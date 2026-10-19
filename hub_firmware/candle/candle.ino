// candle.ino - ESP-NOW candle listener for ESP32-C3 SuperMini.  v2 2026-10-11.
// FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc
// Pins: GPIO3 <- IR receiver OUT (idle high)
//       GPIO4 -> 2N7000 gate -> WICK (top) LED, 120R from 3V3, 100k G-S pulldown   (flicker/steady PWM)
//       GPIO5 -> P4 (150R-1k) -> BASE (inner) LED -> GND                          (steady PWM)
// OFF (both LEDs off): deep sleep, wake every WAKE_S s for one hub broadcast, or on any IR burst.
// ON : stay awake for PWM; radio window every LIT_LISTEN_MS.
// Hub entries are applied only when they CHANGE. Remote presses are reported (reason IR);
// hub B copies them to EVERY candle and runs the group timer.
// 4.6 2026-10-12: long range (LR) radio + one-hop relay - see cp_radio.h and the "4.6 relay" block below.
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "candle_proto.h"
#include "cp_radio.h"
#include <IRremote.hpp>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <driver/ledc.h>
#include <Preferences.h>                            // 4.9: OTA attempt counter in NVS
#include <esp_timer.h>                              // 4.9: OTA hard deadline

#define CANDLE_FW_VER "4.11.1"                         // bump on every release - also changes the image's fwId
const char FW_TAG[] = "@@FWTAG:candle@@";         // hub A's relay only accepts candle images carrying this tag

// v4 rollback: the core would mark a new image valid at boot. We defer that until this firmware has
// actually talked to the hub; if it can't within 90 s it rolls itself back to the previous image.
extern "C" bool verifyRollbackLater() { return true; }

// ---- pins / timing ----
#define PIN_IR          3
#define PIN_WICK        4
#define PIN_BASE        5
#define VBAT_GAIN_PPM   1083000     // 4.7 2026-10-13: DMM 2.36 V vs reported 2.179 V (4.6), cross-checked 2.53/2.30 on 10-11
#define OTA_MIN_MV      2250        // skip firmware pulls on battery below this (true mV). 4.8: 2400; 4.9: 2250 -
                                    // the NVS try-counter now stops loops, and 2400 sat on the NiMH plateau
#define TX_QDBM         34          // 4.9: max TX power in 0.25 dBm units (34 = 8.5 dBm; IDF default ~80 = 20 dBm).
                                    // 10/13: a 2.36 V-rest NiMH pair could not survive the radio start-up surge
                                    // (~0.9 A from the pack via 5 V boost + HT7333). Raise if range suffers.
#define BROWNOUT_SLEEP_S 600        // 4.9: after a brownout reset, stay dark this long before retrying (no loop)
#define PIN_VBAT        1           // 4.2: battery + (after the switch) wired straight to GPIO1 - no divider.
                                    // OK for NiMH/alkaline 2xAA (<=3.2 V). Reads up to ~2.5 V; above that = "full".
#define WAKE_S          10          // off: check-in interval
#define LISTEN_MS       80          // radio window per check (hub sends every 50 ms)
#define LIT_LISTEN_MS   10000       // on: how often to open the radio window (4.4: was 3000 - power)
#define IR_STAY_MS      6000        // after an IR wake, stay up this long for the button press
#define PWM_HZ          64000       // 4.4: RC_FAST clock (~17.5 MHz / 256) so PWM runs in light sleep; still clear of the IR receiver's 38 kHz band
#define CH_WICK         0
#define CH_BASE         1
#define PWM_BITS        8
#define DIM_STEP        40          // Dim+/Dim- step on the 1-255 wick brightness
#define BASE_DEFAULT    160

// ---- flicker tuning (perceived brightness 0-1, gamma 2.2 applied on output). Tune by eye. ----
#define FLK_TICK_MS     40          // update rate
// FLK_FLOOR / FLK_GUST_LEVEL are now set by the hub's group flicker depth (rDepth, HA "Candle flicker" slider)
#define FLK_FLOOR       0.35f       // reference only: depth 72 gives this floor
#define FLK_CEIL        1.00f
#define FLK_NEWTGT_PCT  14          // % chance per tick to pick a new target (v1 18, v2 6) -> ~every 0.3 s
#define FLK_GLIDE       0.18f       // fraction of the gap closed per tick (v1 0.30, v2 0.08) -> livelier
#define FLK_GUST_PERMIL 9           // per-mille chance per tick of a gust (~1 per 4.5 s; v2 ~1 per 8 s)
#define FLK_GUST_LEVEL  0.25f       // gust dip depth
#define FLK_GUST_GLIDE  0.35f       // gusts drop/recover faster
#define FLK_GUST_TICKS  5           // ticks held at the gust target (~200 ms)
#define FLK_BASE_FOLLOW 0.46f       // base follows this fraction of the wick's dip (0 = steady, 1 = same depth)
                                    // 0.46: wick at 0.35 -> base ~0.70; gust 0.25 -> base ~0.66

// ---- remote (stock + SwitchBot, NEC addr 0x80, captured 2026-10-11 via Bond) ----
enum IrAct : uint8_t { A_OFF, A_ON, A_BRIGHTER, A_DIMMER, A_FLICKER, A_STEADY, A_TIMER };
struct IrMap { uint16_t addr; uint8_t cmd; uint8_t act; uint8_t arg; };
static const IrMap IR_MAP[] = {
  {0x0080, 0x12, A_ON,       0},   // ON
  {0x0080, 0x1E, A_OFF,      0},   // OFF
  {0x0080, 0x07, A_FLICKER,  0},   // Candle
  {0x0080, 0x09, A_STEADY,   0},   // Light
  {0x0080, 0x1F, A_BRIGHTER, 0},   // Dim+
  {0x0080, 0x0A, A_DIMMER,   0},   // Dim-
  {0x0080, 0x01, A_TIMER,    2},   // 2H
  {0x0080, 0x03, A_TIMER,    4},   // 4H
  {0x0080, 0x04, A_TIMER,    6},   // 6H
  {0x0080, 0x06, A_TIMER,    8},   // 8H
};

// ---- state kept across deep sleep ----
RTC_DATA_ATTR uint8_t  rFlags = 0, rWB = 255, rBB = BASE_DEFAULT;   // what the LEDs are doing
RTC_DATA_ATTR uint8_t  rHubF = 0xFF, rHubWB = 0, rHubBB = 0;       // last hub entry applied (F=FF: none)
RTC_DATA_ATTR uint8_t  rHubMac[6];
RTC_DATA_ATTR bool     rHaveHub = false;
RTC_DATA_ATTR uint32_t rSecs = 0;                                    // seconds since last daily report
RTC_DATA_ATTR uint32_t rLastHello = 0;
RTC_DATA_ATTR uint8_t  rDepth = CP_DEPTH_DEFAULT;                    // group flicker depth 0-100
RTC_DATA_ATTR uint32_t rOtaFailId = 0;                               // fwId that failed to install...
RTC_DATA_ATTR uint8_t  rOtaFails = 0;                                // ...and how often (stop after 3)
static volatile uint32_t sFwId = 0;                                  // fwId hub B is offering
static bool pendingVerify = false;                                   // running a new image not yet confirmed
static volatile bool otaActive = false;
struct OtaRx { uint8_t len; uint8_t b[sizeof(CpOtaData)]; };
static QueueHandle_t otaRxQ;
static uint32_t myFw() { uint32_t v; memcpy(&v, esp_app_get_description()->app_elf_sha256, 4); return v; }
static volatile bool heardNoEntry = false;
static volatile bool heardNoEntryDirect = false;

// ---- 4.6 relay (2026-10-12) ----
// A LIT candle that hears hub B DIRECTLY re-broadcasts each new hub state (CP_MAGIC_RELAY, LR rate) every
// RELAY_EVERY_MS for RELAY_MS - one full off-candle check-in period - so candles out of B's reach still get it.
// Suppression: wait a random holdoff first; if RELAY_K other candles are already relaying this seq, stay quiet.
// "Far" candles (no direct hub frame for DIRECT_STALE_S) also broadcast their reports; candles that reach B
// forward those to B. One hop only: relay frames and forwarded reports are never relayed again.
// Cost: radio on ~10.5 s per state change on <= RELAY_K+ lit candles (~0.25 mAh each).
#define RELAY_ENABLE    1
#define RELAY_MS        10500
#define RELAY_EVERY_MS  50
#define RELAY_HOLD_MS   400
#define RELAY_K         2
#define RELAY_CAP_MS    30000           // one relay session never keeps the radio up longer than this
#define DIRECT_STALE_S  60
#define UPLINK_MS       10500           // far candle repeats an IR report this long, or until the hub echoes it
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
RTC_DATA_ATTR uint32_t rLastDirect = 0;          // rSecs when hub B was last heard directly
RTC_DATA_ATTR bool     rEverDirect = false;
static volatile bool sDirect = false;            // the entry in sF/sWB/sBB came straight from hub B
static CpState relayBuf;                         // last hub state heard directly, magic already set to RELAY
static volatile uint8_t  relayLen = 0;
static volatile uint16_t relaySeqSeen = 0;
static volatile bool     relayHave = false;
static volatile uint16_t heardRelaySeq = 0;      // other candles' relays of this seq...
static volatile uint8_t  heardRelayN = 0;        // ...from this many distinct candles
static uint8_t heardRelayIds[RELAY_K][3];
static uint16_t relayedSeq = 0; static bool relayedAny = false;
static uint32_t relayFrom = 0, relayUntil = 0, relayStartedAt = 0;   // millis; relayUntil 0 = idle
static QueueHandle_t fwdQ;                       // far candles' reports waiting to be forwarded to B
static CpReport upBuf; static uint32_t upUntil = 0;   // our own IR report, repeated while we depend on relays
static bool farMode() { return !rEverDirect || rSecs - rLastDirect > DIRECT_STALE_S; }

static uint8_t myId[3];
static volatile bool gotState = false;
static volatile uint8_t sF, sWB, sBB, sMac[6];
static bool radioUp = false;
static inline bool lit() { return rFlags & CP_F_ON; }

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
  if (otaActive && len >= 2 && len <= (int)sizeof(CpOtaData) && d[1] == CP_VERSION &&
      (d[0] == CP_MAGIC_OTAINFO || d[0] == CP_MAGIC_OTADATA)) {          // v4 firmware chunks from hub B
    OtaRx r; r.len = len; memcpy(r.b, d, len); xQueueSend(otaRxQ, &r, 0); return;
  }
  if (cpReportLenOk(len) && d[0] == CP_MAGIC_REPORT && d[1] == CP_VERSION) {   // 4.6: a far candle's report (4.11: old or new length)
    if (((const CpReport *)d)->reason & CP_R_RELAYED) { CpReport r = {}; memcpy(&r, d, len); xQueueSend(fwdQ, &r, 0); }
    return;
  }
  bool relay = len >= CP_STATE_HDR && d[0] == CP_MAGIC_RELAY;
  if (len < CP_STATE_HDR || (d[0] != CP_MAGIC_STATE && !relay) || d[1] != CP_VERSION) return;
  const CpState *s = (const CpState *)d;
  if (s->n > CP_MAX_ENTRIES || len < (int)CP_STATE_LEN(s->n)) return;
  if (relay) {                                       // 4.6: count distinct candles relaying this seq (suppression)
    if (s->seq != heardRelaySeq) { heardRelaySeq = s->seq; heardRelayN = 0; }
    bool known = false;
    for (uint8_t k = 0; k < heardRelayN; k++) if (!memcmp(heardRelayIds[k], info->src_addr + 3, 3)) known = true;
    if (!known && heardRelayN < RELAY_K) memcpy(heardRelayIds[heardRelayN++], info->src_addr + 3, 3);
  } else {                                           // straight from hub B
    sFwId = s->fwId;                                 // candle firmware the hub is offering (0 = none)
    memcpy((void *)sMac, info->src_addr, 6);
    memcpy(&relayBuf, d, len); relayBuf.magic = CP_MAGIC_RELAY; relayLen = len; relaySeqSeen = s->seq; relayHave = true;
  }
  if (s->depth <= 100) rDepth = s->depth;            // group flicker depth applies to every candle
  for (int i = 0; i < s->n; i++)
    if (!memcmp(s->e[i].id, myId, 3)) { sF = s->e[i].flags; sWB = s->e[i].wbri; sBB = s->e[i].bbri; sDirect = !relay; gotState = true; return; }
  gotState = false; heardNoEntry = true; heardNoEntryDirect = !relay;   // no entry for us yet
  if (!relay) { memcpy(rHubMac, (const void *)sMac, 6); rHaveHub = true; }  // direct: still learn the hub's MAC
}

static void radioOn() {
  if (radioUp) return;
  WiFi.mode(WIFI_STA);
  cpEnableLR();                                      // 4.6: 11b/g/n + LR
  esp_wifi_set_channel(CP_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(TX_QDBM);               // 4.9: cap TX power - the start-up surge browned out a ~25 %-used pack
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(cpOnSent);
  cpAddPeer(BCAST);
  radioUp = true;
}

static void radioOff() {
  if (!radioUp) return;
  esp_now_deinit(); WiFi.mode(WIFI_OFF); radioUp = false;
}

// Battery pack voltage, read with the radio on (real load). Averaged; < 1000 mV = nothing wired (reported as 0).
// (4.2 used 300 mV: an unwired, floating GPIO1 read ~330 mV and showed up in HA as 0.33 V.)
static uint16_t vbatMv() {                          // 4.6: 32 samples, sorted, mean of the middle 16
  uint16_t v[32];                                    // (trimmed mean: drops PWM/boost spikes; was a plain 8-avg, +-0.13 V)
  for (int i = 0; i < 32; i++) {
    uint16_t x = analogReadMilliVolts(PIN_VBAT); int j = i;
    while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
    v[j] = x;
  }
  uint32_t s = 0; for (int i = 8; i < 24; i++) s += v[i];
  uint16_t mv = s / 16;
  if (mv < 1000) return 0;                         // nothing wired (raw, before the correction)
  return (uint16_t)((mv * (uint32_t)VBAT_GAIN_PPM + 500000) / 1000000);   // 4.7: ADC reads ~8 % low (DMM-calibrated)
}

// 4.6: unicast to hub B at normal AND LR rate (B dedupes). A far candle also broadcasts it (LR) with the
// RELAYED bit so a candle that reaches B can forward it; an IR report (or uplink=true) keeps repeating for
// UPLINK_MS or until the hub's state for us matches (see applyHub).
static void report(uint8_t reason, uint8_t tmrH = 0, bool uplink = false) {
  bool far = RELAY_ENABLE && farMode();
  if (!rHaveHub && !far) return;
  radioOn();
  CpReport r = {CP_MAGIC_REPORT, CP_VERSION, {myId[0], myId[1], myId[2]}, rFlags, rWB, rBB, vbatMv(), reason, tmrH, myFw(), {0}};
  strncpy(r.fw, CANDLE_FW_VER, sizeof(r.fw));       // 4.11: firmware version -> HA (hub B v5.2 / hub A v5.2)
  if (rHaveHub) { cpAddPeer(rHubMac); cpSendBoth(rHubMac, &r, sizeof(r)); }
  if (far) {
    r.reason |= CP_R_RELAYED;
    cpSendAt(BCAST, &r, sizeof(r), true);
    if (reason == CP_R_IR || uplink) { upBuf = r; upUntil = millis() + UPLINK_MS; if (!upUntil) upUntil = 1; }
  }
  Serial.printf("report flags=%u wb=%u bb=%u mv=%u why=%u tmr=%u%s\n", rFlags, rWB, rBB, r.mv, reason, tmrH, far ? " (far: broadcast too)" : "");
}

// 4.6: forward far candles' reports to hub B (only if WE reach B directly). Dedupe: they repeat every 50 ms.
static void fwdTick() {
  CpReport r;
  static CpReport last; static uint32_t lastMs = 0;
  while (xQueueReceive(fwdQ, &r, 0) == pdTRUE) {
    if (!RELAY_ENABLE || !rHaveHub || farMode() || !radioUp) continue;
    if (!memcmp(&r, &last, sizeof(r)) && millis() - lastMs < 2000) continue;
    last = r; lastMs = millis();
    cpAddPeer(rHubMac); cpSendBoth(rHubMac, &r, sizeof(r));
    Serial.printf("fwd report from %02X%02X%02X why=%u\n", r.id[0], r.id[1], r.id[2], r.reason & 0x7F);
  }
}

static inline uint32_t gammaDuty(float lin) { return (uint32_t)(255.0f * powf(constrain(lin, 0.0f, 1.0f), 2.2f) + 0.5f); }

// 4.4: LED PWM via the IDF driver - RC_FAST clock + KEEP_ALIVE so both LEDs keep running while the CPU light-sleeps.
static bool pwmInit() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE; t.duty_resolution = LEDC_TIMER_8_BIT; t.timer_num = LEDC_TIMER_0;
  t.freq_hz = PWM_HZ; t.clk_cfg = LEDC_USE_RC_FAST_CLK;
  if (ledc_timer_config(&t) != ESP_OK) return false;
  const int pins[2] = {PIN_WICK, PIN_BASE};
  for (int i = 0; i < 2; i++) {
    ledc_channel_config_t c = {};
    c.gpio_num = pins[i]; c.speed_mode = LEDC_LOW_SPEED_MODE; c.channel = (ledc_channel_t)i;
    c.timer_sel = LEDC_TIMER_0; c.duty = 0; c.hpoint = 0; c.sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE;
    if (ledc_channel_config(&c) != ESP_OK) return false;
  }
  return true;
}
static inline void pwmWrite(uint8_t ch, uint32_t d) { ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, d); ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch); }
static inline uint32_t pwmRead(uint8_t ch) { return ledc_get_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch); }

static void baseWrite(float f = 1.0f) { pwmWrite(CH_BASE, (rFlags & CP_F_BASE) ? gammaDuty((rBB / 255.0f) * f) : 0); }
static void wickWrite(float f) { pwmWrite(CH_WICK, (rFlags & CP_F_WICK) ? gammaDuty((rWB / 255.0f) * f) : 0); }
static void ledsDark() { pwmWrite(CH_WICK, 0); pwmWrite(CH_BASE, 0); }

// Wick: slow random walk between FLK_FLOOR and FLK_CEIL, plus occasional quick "gust" dips.
static void flickerStep() {
  static float level = 0.85f, target = 0.85f;
  static uint8_t gust = 0;
  if (rFlags & CP_F_STEADY) { level = target = 1.0f; gust = 0; wickWrite(1.0f); baseWrite(1.0f); return; }
  float glide = FLK_GLIDE;
  float flo = 1.0f - rDepth * 0.009f;                    // depth 0-100 -> floor 1.00-0.10 (72 -> 0.35)
  float gustLvl = max(0.05f, flo - 0.10f);               // gusts dip a bit below the floor
  if (gust) { gust--; glide = FLK_GUST_GLIDE; if (!gust) target = flo + random(101) * (FLK_CEIL - flo) / 100.0f; }
  else if (random(1000) < FLK_GUST_PERMIL) { gust = FLK_GUST_TICKS; target = gustLvl; glide = FLK_GUST_GLIDE; }
  else if (random(100) < FLK_NEWTGT_PCT) target = flo + random(101) * (FLK_CEIL - flo) / 100.0f;
  level += (target - level) * glide;
  wickWrite(level);
  baseWrite(1.0f - (1.0f - level) * FLK_BASE_FOLLOW);   // inner glow rides the flame, shallower
}

// ---- v4 firmware update over ESP-NOW: pull from hub B in windows of CP_OTA_WINDOW chunks ----
static void verifyOk() {                           // new image reached the hub: keep it
  if (!pendingVerify) return;
  esp_ota_mark_app_valid_cancel_rollback(); pendingVerify = false;
  Serial.printf("new firmware %s (%08lX) verified\n", CANDLE_FW_VER, (unsigned long)myFw());
  report(CP_R_FWOK);
}
static void verifyTimeout() {                      // new image can't reach the hub: go back to the old one
  if (pendingVerify && millis() > 90000) {
    Serial.println("new firmware never reached the hub - rolling back"); Serial.flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

// ---- 4.9 OTA fail-safe (2026-10-13, after 4.6's client hung on a pull: dark candle, no reports) ----
// 1. Tries are counted in NVS (flash) BEFORE each pull, so a reset, brownout or hang can't lose the count.
// 2. A one-shot esp_timer restarts the candle if a pull runs past OTA_DEADLINE_MS (catches hangs).
// 3. After OTA_MAX_TRIES failed tries of one image the candle stops pulling it and just runs.
// 4. The flame stays on (steady) during a pull and both LEDs are restored after a failure.
#define OTA_DEADLINE_MS 300000          // 4.11: 5 min (was 3) - hub B v5.2 lets up to 9 candles download at once
static uint32_t otaBusyUntil = 0;       // 4.11: hub said "busy" - don't ask again before this (millis)
#define OTA_MAX_TRIES   3
static Preferences otaNvs;
static esp_timer_handle_t otaDlTimer = nullptr;
static void otaDlCb(void *) { esp_restart(); }       // runs in the esp_timer task, so it fires even if loop() is stuck
static void otaTriesGet(uint32_t &fw, uint8_t &n) {
  otaNvs.begin("ota", true); fw = otaNvs.getUInt("fw", 0); n = otaNvs.getUChar("n", 0); otaNvs.end();
}
static void otaTriesSet(uint32_t fw, uint8_t n) {
  otaNvs.begin("ota", false); otaNvs.putUInt("fw", fw); otaNvs.putUChar("n", n); otaNvs.end();
}
static void otaDeadline(bool arm) {
  if (!otaDlTimer) { esp_timer_create_args_t a = {}; a.callback = otaDlCb; a.name = "otaDL"; esp_timer_create(&a, &otaDlTimer); }
  esp_timer_stop(otaDlTimer);
  if (arm) esp_timer_start_once(otaDlTimer, (uint64_t)OTA_DEADLINE_MS * 1000ULL);
}
static void otaLedsBack() { if (lit()) { baseWrite(); wickWrite(1.0f); } else ledsDark(); }

static void otaFailed(uint32_t fw, const char *why, bool begun) {
  if (begun) Update.abort();
  otaActive = false; otaDeadline(false); otaLedsBack();
  uint32_t f; uint8_t n; otaTriesGet(f, n);
  Serial.printf("OTA %08lX failed (%s), try %u of %u\n", (unsigned long)fw, why, n, OTA_MAX_TRIES);
}

static void otaReq(uint32_t fw, uint32_t off, uint8_t cnt) {
  cpAddPeer(rHubMac);
  CpOtaReq q = {CP_MAGIC_OTAREQ, CP_VERSION, fw, off, cnt};
  // 4.10: plain send, exactly like 4.5 (whose pulls worked). 4.6-4.9 used cpSendAt() here = rate-config call +
  // up-to-25 ms TX wait on EVERY request -> suspected cause of the 10/13 pull crawl/hang. Rate is set once per pull.
  esp_now_send(rHubMac, (const uint8_t *)&q, sizeof(q));
}
static void otaHubRateNormal() {                    // 4.10: once per pull - reports leave the hub peer at LR (cpSendBoth)
  cpAddPeer(rHubMac);
  esp_now_rate_config_t c = {}; c.phymode = WIFI_PHY_MODE_11B; c.rate = WIFI_PHY_RATE_1M_L;
  esp_now_set_peer_rate_config(rHubMac, &c);
}

static void candleOta(uint32_t fw) {
  Serial.printf("OTA: hub offers %08lX, running %08lX (%s) - pulling\n", (unsigned long)fw, (unsigned long)myFw(), CANDLE_FW_VER);
  otaLedsBack();                                    // 4.9: flame stays on (steady - flicker pauses during a pull)
  xQueueReset(otaRxQ); otaActive = true; radioOn(); otaDeadline(true); otaHubRateNormal();
  uint32_t tA = millis();
  OtaRx r; CpOtaInfo info = {}; bool got = false, busy = false;
  for (int t = 0; t < 5 && !got && !busy; t++) {   // INFO: size + md5
    otaReq(fw, 0, 0); uint32_t t0 = millis();
    while (!got && !busy && millis() - t0 < 300)
      if (xQueueReceive(otaRxQ, &r, pdMS_TO_TICKS(20)) == pdTRUE && r.b[0] == CP_MAGIC_OTAINFO) {
        memcpy(&info, r.b, sizeof(info));
        if (info.fwId == fw && !info.size) busy = true;   // 4.11: hub B v5.2 "busy" (download slots full)
        else got = (info.fwId == fw && info.size);
      }
  }
  if (busy) {                                       // 4.11: not a failure - give the try back, ask again later
    otaActive = false; otaDeadline(false); otaLedsBack();
    uint32_t f; uint8_t n; otaTriesGet(f, n); if (f == fw && n) otaTriesSet(fw, n - 1);
    otaBusyUntil = millis() + 60000 + (uint32_t)random(30000);
    Serial.printf("OTA %08lX: hub busy - asking again in ~1 min (try not counted)\n", (unsigned long)fw);
    return;
  }
  if (!got) return otaFailed(fw, "no INFO from hub", false);
  Serial.printf("OTA info: %lu bytes (+%lu ms) - Update.begin\n", (unsigned long)info.size, (unsigned long)(millis() - tA));
  char md5[33]; for (int i = 0; i < 16; i++) sprintf(md5 + i * 2, "%02x", info.md5[i]);
  if (!Update.begin(info.size)) return otaFailed(fw, Update.errorString(), false);
  Update.setMD5(md5);
  Serial.printf("OTA begin ok (+%lu ms) - pulling data\n", (unsigned long)(millis() - tA));
  uint32_t off = 0, lastProg = millis(), t0 = millis(), hb = millis(); uint8_t pct = 0;
  while (off < info.size) {
    if (millis() - hb > 5000) { hb = millis(); Serial.printf("OTA t=%lus off=%lu/%lu q=%u\n", (unsigned long)((millis() - t0) / 1000), (unsigned long)off, (unsigned long)info.size, (unsigned)uxQueueMessagesWaiting(otaRxQ)); }
    otaReq(fw, off, CP_OTA_WINDOW);
    uint8_t n = 0; uint32_t w0 = millis();
    while (n < CP_OTA_WINDOW && off < info.size && millis() - w0 < 250) {
      if (xQueueReceive(otaRxQ, &r, pdMS_TO_TICKS(20)) != pdTRUE || r.b[0] != CP_MAGIC_OTADATA) continue;
      CpOtaData *dt = (CpOtaData *)r.b;
      if (dt->fwId != fw || dt->off != off || dt->len == 0) continue;   // out of order: re-asked next window
      if (Update.write(dt->d, dt->len) != dt->len) return otaFailed(fw, Update.errorString(), true);
      off += dt->len; n++; lastProg = w0 = millis();
    }
    if (millis() - lastProg > 10000) return otaFailed(fw, "stalled", true);
    uint8_t p = (uint64_t)off * 100 / info.size;
    if (p >= pct + 10) { pct = p - p % 10; Serial.printf("OTA %u%%\n", pct); }
  }
  if (!Update.end(true)) return otaFailed(fw, Update.errorString(), true);   // MD5 checked here
  otaActive = false;
  Serial.printf("OTA ok: %lu bytes in %lu s - rebooting into new firmware\n", (unsigned long)info.size, (unsigned long)((millis() - t0) / 1000));
  Serial.flush(); delay(200); ESP.restart();
}

static void otaCheck() {                           // called whenever we have just heard the hub
  uint32_t fw = sFwId;
  if (!fw || fw == myFw() || pendingVerify || !rHaveHub) return;
  if (otaBusyUntil && (int32_t)(millis() - otaBusyUntil) < 0) return;   // 4.11: hub busy - wait (resets in deep sleep: cheap re-ask)
  uint32_t tf; uint8_t tn; otaTriesGet(tf, tn);     // 4.9: try counter lives in NVS (survives resets/hangs)
  if (tf == fw && tn >= OTA_MAX_TRIES) {
    static bool said = false; if (!said) { said = true; Serial.printf("OTA %08lX: gave up after %u tries (new upload resets)\n", (unsigned long)fw, tn); }
    return;
  }
  // 4.8: no firmware pull on a very low pack (pull = radio on ~1 min + flash writes). 10/13 lesson: the dark,
  // looping candle was 4.6's OTA client HANGING (also on USB), not only the battery. vb == 0 (USB / no sense) -> allowed.
  uint16_t vb = vbatMv();
  if (vb && vb < OTA_MIN_MV) {
    static uint32_t lastWarn = 0;
    if (!lastWarn || millis() - lastWarn > 600000) { lastWarn = millis(); Serial.printf("OTA %08lX waiting: battery %u mV < %u\n", (unsigned long)fw, vb, OTA_MIN_MV); }
    return;
  }
  tn = (tf == fw) ? tn + 1 : 1;
  otaTriesSet(fw, tn);                              // counted BEFORE the pull: a reset mid-pull still counts
  Serial.printf("OTA %08lX: try %u of %u\n", (unsigned long)fw, tn, OTA_MAX_TRIES);
  candleOta(fw);
}

// Apply our entry from the last frame (direct or relayed) if it changed. gotState must be set.
static void applyHub() {
  bool direct = sDirect;
  if (direct) { memcpy(rHubMac, (const void *)sMac, 6); rHaveHub = true; rLastDirect = rSecs; rEverDirect = true; }
  if (upUntil && sF == rFlags && sWB == rWB && sBB == rBB) { upUntil = 0; Serial.println("uplink: hub has our state"); }
  if (sF != rHubF || sWB != rHubWB || sBB != rHubBB) {   // hub entry changed since we last applied it
    rHubF = sF; rHubWB = sWB; rHubBB = sBB;
    rFlags = sF; rWB = sWB ? sWB : 255; rBB = sBB ? sBB : BASE_DEFAULT;
    baseWrite();
    Serial.printf("hub says flags=%u wb=%u bb=%u%s\n", rFlags, rWB, rBB, direct ? "" : " (via relay)");
    report(CP_R_APPLIED);
  }
  if (direct) {
    verifyOk();                                    // v4: a new image that hears the hub is confirmed
    otaCheck();                                    // v4: hub offering different firmware -> pull it (reboots on success)
  }
}

// Open the radio, wait for one hub (or relay) broadcast, apply our entry if it changed. Returns true if heard.
static bool listenOnce(uint32_t ms) {
  gotState = false; heardNoEntry = false; radioOn();
  uint32_t t0 = millis();
  while (millis() - t0 < ms && !gotState) delay(2);
  if (!gotState) {
    if (heardNoEntry) {
      Serial.printf("hub heard%s, no entry for us\n", heardNoEntryDirect ? "" : " (via relay)");
      if (heardNoEntryDirect) { rLastDirect = rSecs; rEverDirect = true; verifyOk(); otaCheck(); }
    }
    fwdTick();
    return false;
  }
  gotState = false;
  applyHub();
  fwdTick();
  return true;
}

// 4.6: lit + heard a NEW hub state directly -> relay it (after a random holdoff, unless RELAY_K others already are).
static void relayTick(uint32_t now) {
  if (!RELAY_ENABLE || !lit()) { relayUntil = 0; return; }
  if (relayHave && (!relayedAny || relaySeqSeen != relayedSeq)) {
    relayedAny = true; relayedSeq = relaySeqSeen;
    if (!relayUntil) { relayStartedAt = now; relayFrom = now + random(RELAY_HOLD_MS); }
    relayUntil = now + RELAY_MS;
    if ((int32_t)(relayUntil - (relayStartedAt + RELAY_CAP_MS)) > 0) relayUntil = relayStartedAt + RELAY_CAP_MS;
    if (!relayUntil) relayUntil = 1;
    Serial.printf("relay: seq %u\n", relayedSeq);
  }
  if (!relayUntil) return;
  if ((int32_t)(now - relayUntil) >= 0) { relayUntil = 0; Serial.println("relay: done"); return; }
  if ((int32_t)(now - relayFrom) < 0) {            // holdoff: listen for other relays of this seq
    if (heardRelaySeq == relayedSeq && heardRelayN >= RELAY_K) { relayUntil = 0; Serial.println("relay: suppressed"); }
    return;
  }
  static uint32_t lastTx = 0;
  if (now - lastTx < RELAY_EVERY_MS) return;
  lastTx = now;
  CpState f; uint8_t n = relayLen; memcpy(&f, &relayBuf, n);
  cpSendAt(BCAST, &f, n, true);
}

// 4.6: far candle repeats its own IR report until the hub's state echoes it (relayed) or UPLINK_MS runs out.
static void uplinkTick(uint32_t now) {
  if (!upUntil) return;
  if ((int32_t)(now - upUntil) >= 0) { upUntil = 0; Serial.println("uplink: no echo from hub"); return; }
  static uint32_t lastTx = 0;
  if (now - lastTx < RELAY_EVERY_MS) return;
  lastTx = now; radioOn();
  cpSendAt(BCAST, &upBuf, sizeof(upBuf), true);
}

// Acknowledge a remote press on the candle that received it.
// Lit: LEDs dark 120 ms then back, n times. Off: brief flash on, n times.
#define ACK_MS     120
#define ACK_GAP_MS 180
static void ackBlink(uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (lit()) { ledsDark(); delay(ACK_MS); wickWrite(1.0f); baseWrite(1.0f); }
    else { uint32_t d = gammaDuty(0.6f); pwmWrite(CH_WICK, d); pwmWrite(CH_BASE, d); delay(ACK_MS); ledsDark(); }
    if (i + 1 < n) delay(ACK_GAP_MS);
  }
}

static void irAction(uint8_t act, uint8_t arg) {
  uint8_t pF = rFlags, pW = rWB, pB = rBB, tmr = 0;
  switch (act) {
    case A_OFF:      rFlags &= ~CP_F_ON; break;
    case A_ON:       rFlags |= CP_F_ON; break;
    case A_FLICKER:  rFlags = (rFlags & ~CP_F_STEADY) | CP_F_ON; break;
    case A_STEADY:   rFlags |= CP_F_STEADY | CP_F_ON; break;
    case A_BRIGHTER: rWB = min(255, rWB + DIM_STEP); rBB = min(255, rBB + DIM_STEP); rFlags |= CP_F_ON; break;   // wick + base together
    case A_DIMMER:   rWB = max(25, rWB - DIM_STEP);  rBB = max(25, rBB - DIM_STEP); break;
    case A_TIMER:    rFlags |= CP_F_ON; tmr = arg; break;
  }
  baseWrite(); if (!(rFlags & CP_F_WICK)) pwmWrite(CH_WICK, 0);
  uint8_t blinks = (act == A_OFF) ? 0 : (act == A_TIMER ? arg / 2 : 1);   // timer 2/4/6/8 h -> 1/2/3/4 blinks
  if (blinks) ackBlink(blinks);
  if (rFlags != pF || rWB != pW || rBB != pB || tmr) { Serial.printf("IR -> flags=%u wb=%u tmr=%u\n", rFlags, rWB, tmr); report(CP_R_IR, tmr); }
}

static bool pollIr() {
  if (!IrReceiver.decode()) return false;
  auto &d = IrReceiver.decodedIRData;
  bool handled = false;
  static uint32_t lastRaw = 0, lastMs = 0;
  bool dup = d.decodedRawData == lastRaw && millis() - lastMs < 600;   // burst of identical frames = one press
  lastRaw = d.decodedRawData; lastMs = millis();
  if (!(d.flags & IRDATA_FLAGS_IS_REPEAT) && !dup) {
    Serial.printf("IR %s addr=0x%04X cmd=0x%02X raw=0x%08lX bits=%u\n", getProtocolString(d.protocol), d.address, d.command, (unsigned long)d.decodedRawData, d.numberOfBits);
    if (d.protocol == UNKNOWN && d.rawlen > 8) { Serial.print("IR raw: "); IrReceiver.printIRResultRawFormatted(&Serial, true); }
    for (auto &m : IR_MAP)
      if (m.addr == d.address && m.cmd == d.command) { irAction(m.act, m.arg); handled = true; }
  }
  IrReceiver.resume();
  return handled;
}

// 4.5: periodic status report (battery, RSSI, last-seen) - every 30 min lit, every 60 min off - so HA gets a
// battery trend. Was once a day. rSecs counts awake seconds plus WAKE_S per check-in.
RTC_DATA_ATTR uint32_t rLastStatus = 0;
static void maybeDaily() {
  uint32_t every = lit() ? 1800UL : 3600UL;
  if (rSecs - rLastStatus >= every) { rLastStatus = rSecs; report(CP_R_DAILY); }
}

static void announceIfUnknown() {          // hub has no line for us -> (re)introduce, max once a minute
  if (heardNoEntry && (rHaveHub || farMode()) && (rSecs - rLastHello >= 60 || rLastHello == 0)) { rLastHello = rSecs ? rSecs : 1; report(CP_R_BOOT); }
}

// 4.4: light-sleep the CPU until the next flicker step. LEDs keep running (KEEP_ALIVE PWM); an IR edge wakes it
// early and keeps it awake 250 ms so IRremote can decode the whole frame. Never naps on USB (bench logs),
// with the radio on, or while an IR frame is being received.
static uint32_t noNapUntil = 0;
static void nap(uint32_t ms) {
  if (ms < 4 || Serial || radioUp || !IrReceiver.isIdle() || (int32_t)(millis() - noNapUntil) < 0) { delay(ms < 5 ? ms : 5); return; }
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  gpio_wakeup_enable((gpio_num_t)PIN_IR, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) noNapUntil = millis() + 250;
}

// Awake loop: used while lit, and for a few seconds after an IR wake.
static void awakeLoop(uint32_t stayMs) {
  uint32_t start = millis(), lastFlick = 0, lastListen = millis(), lastTick = millis();
  baseWrite();
  while (true) {
    uint32_t now = millis();
    if (now - lastFlick >= FLK_TICK_MS) { lastFlick = now; flickerStep(); }
    if (pollIr()) start = now;                       // keep awake while buttons are being pressed
    if (now - lastListen >= LIT_LISTEN_MS) {
      lastListen = now; listenOnce(LISTEN_MS); announceIfUnknown();
    }
    uplinkTick(now);                                 // 4.6: far candle repeating its IR report (turns the radio on)
    if (radioUp) {                                   // 4.6: radio stays up while relaying / uplinking
      if (gotState) { gotState = false; applyHub(); }
      fwdTick(); relayTick(now);
      if (!relayUntil && !upUntil) radioOff();
    }
    if (now - lastTick >= 1000) { lastTick += 1000; rSecs++; maybeDaily(); verifyTimeout();
      Serial.printf("lit: flags=%u wb=%u bb=%u duty w=%u b=%u\n", rFlags, rWB, rBB, (unsigned)pwmRead(CH_WICK), (unsigned)pwmRead(CH_BASE)); }
    if (!lit() && now - start >= stayMs && !upUntil) { ledsDark(); radioOff(); return; }   // off and idle -> dark now, then sleep
    uint32_t due = lastFlick + FLK_TICK_MS, n2 = millis();
    nap((int32_t)(due - n2) > 0 ? due - n2 : 0);   // 4.4: sleep until the next flicker step
  }
}

static void goToSleep() {
  if (Serial) {                              // bench mode: USB host attached -> stay awake and log
    Serial.println("USB attached: bench mode, not sleeping");
    uint32_t last = 0;
    while (Serial) {
      if (millis() - last >= 2000) {
        last = millis();
        bool h = listenOnce(LISTEN_MS);
        Serial.printf("listen: %s  flags=%u wb=%u bb=%u hub=%s\n", h ? "got entry" : "no entry", rFlags, rWB, rBB, rHaveHub ? "known" : "unknown");
        rSecs += 2; announceIfUnknown(); verifyTimeout();
        if (lit()) { radioOff(); awakeLoop(0); }
      }
      pollIr(); if (!lit()) ledsDark(); else { radioOff(); awakeLoop(0); }
      delay(10);
    }
  }
  while (pendingVerify) {                          // never deep-sleep an unconfirmed image (the wake would roll it back)
    listenOnce(LISTEN_MS); radioOff(); verifyTimeout(); delay(1000);
  }
  ledsDark();
  radioOff();
  rSecs += WAKE_S;
  Serial.printf("sleep %ds\n", WAKE_S); Serial.flush();
  IrReceiver.stop();
  esp_sleep_enable_timer_wakeup((uint64_t)WAKE_S * 1000000ULL);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_IR, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
  memcpy(myId, mac + 3, 3);
  setCpuFrequencyMhz(80);                          // 4.4: power - 80 MHz is the radio's minimum and plenty for flicker
  esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON);   // keep the PWM clock alive in light sleep
  bool la = pwmInit(), lb = la;
  ledsDark();
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {      // 4.9: pack can't carry a radio start: stay dark, retry later, never loop
    Serial.printf("brownout reset - dark for %u s, then retry (weak battery?)\n", BROWNOUT_SLEEP_S); Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)BROWNOUT_SLEEP_S * 1000000ULL);
    esp_deep_sleep_start();
  }
  otaRxQ = xQueueCreate(16, sizeof(OtaRx));
  fwdQ = xQueueCreate(8, sizeof(CpReport));        // 4.6: must exist before the radio comes up (onRecv uses it)
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);     // widest input range (~0-2.5 V calibrated)
  { esp_ota_img_states_t s;                        // first boot of a freshly installed image?
    pendingVerify = esp_ota_get_state_partition(esp_ota_get_running_partition(), &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY; }
  IrReceiver.begin(PIN_IR, DISABLE_LED_FEEDBACK);
  esp_sleep_wakeup_cause_t why = esp_sleep_get_wakeup_cause();

  if (why == ESP_SLEEP_WAKEUP_UNDEFINED) {           // power-on / reset
    delay(1500);                                     // time for the USB serial monitor to attach
    Serial.printf("\ncandle %02X%02X%02X fw %s (%08lX)%s boot, ledcAttach wick=%d base=%d\n", myId[0], myId[1], myId[2],
                  CANDLE_FW_VER, (unsigned long)myFw(), pendingVerify ? " NEW-UNVERIFIED" : "", la, lb);
    Serial.printf("fw tag %s\n", FW_TAG);
    for (int i = 0; i < 5 && !rHaveHub; i++) listenOnce(250);
    Serial.printf("hub %s%s\n", rHaveHub ? "found" : "NOT heard", farMode() ? " (far: relying on relays)" : "");
    report(CP_R_BOOT, 0, true);                    // 4.6: a far candle keeps announcing for UPLINK_MS
  } else if (why == ESP_SLEEP_WAKEUP_GPIO) {        // IR burst woke us; the button press itself is lost
    awakeLoop(IR_STAY_MS);
  } else {                                           // timer check-in
    listenOnce(LISTEN_MS);
    announceIfUnknown();
    maybeDaily();
  }
  radioOff();
  if (lit() || upUntil) awakeLoop(0);              // 4.6: also stay up to finish a far candle's uplink
  goToSleep();
}

void loop() {}
