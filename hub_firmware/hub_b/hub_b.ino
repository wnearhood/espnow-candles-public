// hub_b.ino - Candle hub, board B: dedicated ESP-NOW radio. Never joins WiFi.
// v2 2026-10-11: wick/base/mode per candle; IR on ANY candle -> ALL candles; group timers live here
// (not on A) so remote + timers keep working with WiFi/MQTT/HA down.
// Board: ESP32-DevKitC-32E (FQBN esp32:esp32:esp32)
// UART to board A: RX = GPIO16 (from A GPIO17), TX = GPIO17 (to A GPIO16), 115200 8N1
//
// Lines FROM board A:
//   CMD <id6hex> <flags> <wbri> <bbri>   set one candle
//   CMD ALL <flags> <wbri> <bbri>        set every known candle
//   DEPTH <0-100>                        group flicker depth (v3)
//   PING                                 -> PONG <entries> <seq> <timer_s_left>
// Lines TO board A:
//   HELLO <mac>                          after boot (A re-sends all CMDs)
//   RPT <id6hex> <flags> <wbri> <bbri> <mv> <rssi> <reason>
//   ALL <flags> <wbri> <bbri> <reason>   B changed every candle itself (4 = remote, 5 = timer)
//
// v5 2026-10-12: radio = 11b/g/n + long range (LR). Every state frame goes out twice, normal rate (v4 candles)
// then LR (v4.6+ candles that are far away). Accepts reports at either rate, including far candles' reports
// forwarded by relay candles (CP_R_RELAYED; RSSI reported as -127 = "via relay"). Dedupes reports, because v4.6
// candles send each one twice (both rates) and relays may forward copies. Candle firmware OTA stays normal rate.
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <Update.h>
#include <esp_rom_crc.h>
#include <MD5Builder.h>
#include <esp_partition.h>
static char macStr[18];
#include "candle_proto.h"
#include "cp_radio.h"
const char FW_TAG[] = "@@FWTAG:hub_b@@";        // hub A's relay only accepts images carrying this tag

// ---- v3.1 firmware update from hub A over the UART (no WiFi). See hub_a.ino relay section. ----
static bool otaMode = false;
static uint32_t otaExpect = 0, otaGot = 0, otaLastByte = 0, fByteT = 0;
static uint16_t otaSeq = 0, fSeq = 0, fLen = 0, fPos = 0;
static uint8_t fState = 0, fBuf[1024 + 4];
static uint8_t otaKind = 0;                       // 0 = OTAB (B's own firmware), 1 = OTAC (candle image to store)

// ---- v4: candle image store (B's unused SPIFFS partition) + ESP-NOW server ----
struct CMeta { uint32_t magic, size; uint8_t md5[16]; uint32_t fwId; };
#define CMETA_MAGIC 0xC0FFEE42
static const esp_partition_t *cstg = nullptr;
static CMeta cmeta = {};
static uint32_t cErased = 0;
static char cMd5Want[33];
struct OtaRq { uint8_t mac[6]; CpOtaReq r; };
static QueueHandle_t otaQ;

#define LINK Serial2
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static CpState st;
static QueueHandle_t rptQ;
static uint32_t tmrEnd = 0;                       // millis() when the group timer fires (0 = none)

struct Rpt { CpReport r; int8_t rssi; };

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
  if (len == sizeof(CpOtaReq) && d[0] == CP_MAGIC_OTAREQ && d[1] == CP_VERSION) {   // v4 firmware pull
    OtaRq q; memcpy(q.mac, info->src_addr, 6); memcpy(&q.r, d, sizeof(CpOtaReq));
    xQueueSend(otaQ, &q, 0); return;
  }
  if (!cpReportLenOk(len) || d[0] != CP_MAGIC_REPORT || d[1] != CP_VERSION) return;
  Rpt x; memset(&x.r, 0, sizeof(CpReport)); memcpy(&x.r, d, len);   // v5.2: old 12-byte reports -> fwId 0
  x.rssi = (info && info->rx_ctrl) ? info->rx_ctrl->rssi : 0;
  if (x.r.reason & CP_R_RELAYED) {                 // v5: far candle's report
    x.r.reason &= ~CP_R_RELAYED;
    bool bcast = info && info->des_addr && info->des_addr[0] == 0xFF;   // broadcast = heard from the far candle itself
    if (!bcast) x.rssi = -127;                       // unicast = forwarded by a relay candle: real RSSI unknown
  }
  xQueueSend(rptQ, &x, 0);
}

// v5: same report seen in the last 2 s (dual-rate send, relay copies) -> drop.
static bool dupReport(const CpReport &r) {
  static struct { CpReport r; uint32_t ms; } seen[8]; static uint8_t next = 0;
  uint32_t now = millis();
  for (auto &s : seen) if (s.ms && now - s.ms < 2000 && !memcmp(&s.r, &r, sizeof(r))) return true;
  seen[next].r = r; seen[next].ms = now ? now : 1; next = (next + 1) % 8;
  return false;
}

static int findEntry(const uint8_t id[3]) {
  for (int i = 0; i < st.n; i++) if (!memcmp(st.e[i].id, id, 3)) return i;
  return -1;
}

static void setEntry(const uint8_t id[3], uint8_t flags, uint8_t wb, uint8_t bb) {
  if (!wb) wb = 255; if (!bb) bb = 255;
  int i = findEntry(id);
  if (i < 0) {
    if (st.n >= CP_MAX_ENTRIES) { Serial.println("table full"); return; }
    i = st.n++; memcpy(st.e[i].id, id, 3);
  } else if (st.e[i].flags == flags && st.e[i].wbri == wb && st.e[i].bbri == bb) return;   // no change
  st.e[i].flags = flags; st.e[i].wbri = wb; st.e[i].bbri = bb; st.seq++;
}

// Copy one state onto every known candle and tell A, so HA follows.
static void setAll(uint8_t flags, uint8_t wb, uint8_t bb, uint8_t why) {
  for (int i = 0; i < st.n; i++) setEntry(st.e[i].id, flags, wb, bb);
  LINK.printf("ALL %u %u %u %u\n", flags, wb, bb, why);
  Serial.printf("ALL flags=%u wb=%u bb=%u why=%u -> seq %u\n", flags, wb, bb, why, st.seq);
}

static bool parseId(const char *s, uint8_t id[3]) {
  if (strlen(s) != 6) return false;
  for (int k = 0; k < 3; k++) {
    char b[3] = {s[k * 2], s[k * 2 + 1], 0}; char *end;
    id[k] = (uint8_t)strtoul(b, &end, 16); if (*end) return false;
  }
  return true;
}

static uint32_t timerLeftS() { return tmrEnd ? (uint32_t)((int32_t)(tmrEnd - millis()) / 1000) : 0; }

static void otaAbort(const char *why) {
  if (otaKind == 0) Update.abort();
  otaMode = false; fState = 0;
  LINK.printf("OTAFAIL %s\n", why); Serial.printf("OTA fail: %s\n", why);
  LINK.printf("HELLO %s\n", macStr);              // ask A to resend the candle table (old firmware keeps running)
}

// Candle image store: meta lives in the partition's last 4 KB sector.
static uint32_t cMetaOff() { return cstg->size - 4096; }
static void cstoreLoad() {
  cstg = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
  if (!cstg) { Serial.println("no candle store partition"); return; }
  esp_partition_read(cstg, cMetaOff(), &cmeta, sizeof(cmeta));
  if (cmeta.magic != CMETA_MAGIC || cmeta.size == 0 || cmeta.size > cMetaOff()) { memset(&cmeta, 0, sizeof(cmeta)); }
  st.fwId = cmeta.fwId;
  Serial.printf("candle store: %s fwId %08lX size %lu\n", cmeta.magic ? "image" : "empty", (unsigned long)cmeta.fwId, (unsigned long)cmeta.size);
}

static bool otaWrite(const uint8_t *b, uint16_t n) {
  if (otaKind == 0) { if (Update.write((uint8_t *)b, n) != n) { otaAbort(Update.errorString()); return false; } return true; }
  if (otaGot + n > cMetaOff()) { otaAbort("candle image too big"); return false; }
  while (cErased < otaGot + n) { esp_partition_erase_range(cstg, cErased, 4096); cErased += 4096; }
  if (esp_partition_write(cstg, otaGot, b, n) != ESP_OK) { otaAbort("store write"); return false; }
  return true;
}

static void otaFinish() {
  if (otaKind == 0) {
    if (Update.end(true)) {                        // MD5 checked here; new image only boots if it passed
      LINK.println("OTADONE"); Serial.println("OTA done, rebooting"); LINK.flush(); delay(300); ESP.restart();
    } else otaAbort(Update.errorString());
    return;
  }
  // Candle image: re-read what we stored, check MD5 and that it is a real app image, then publish its fwId.
  MD5Builder m; m.begin(); uint8_t buf[1024];
  for (uint32_t o = 0; o < otaExpect; o += sizeof(buf)) {
    uint32_t k = min((uint32_t)sizeof(buf), otaExpect - o); esp_partition_read(cstg, o, buf, k); m.add(buf, k);
  }
  m.calculate();
  if (strcasecmp(m.toString().c_str(), cMd5Want)) { otaAbort("candle image md5 mismatch"); return; }
  uint32_t magic = 0, fw = 0;
  esp_partition_read(cstg, 0x20, &magic, 4); esp_partition_read(cstg, 0xB0, &fw, 4);
  if (magic != 0xABCD5432 || !fw) { otaAbort("not an app image"); return; }
  cmeta.magic = CMETA_MAGIC; cmeta.size = otaExpect; cmeta.fwId = fw; m.getBytes(cmeta.md5);
  esp_partition_erase_range(cstg, cMetaOff(), 4096); esp_partition_write(cstg, cMetaOff(), &cmeta, sizeof(cmeta));
  st.fwId = fw; st.seq++; otaMode = false;
  LINK.printf("OTADONE candle fwId %08lX\n", (unsigned long)fw);
  Serial.printf("candle image stored: %lu bytes fwId %08lX - announcing\n", (unsigned long)otaExpect, (unsigned long)fw);
}

static void otaFrame() {
  uint32_t crc; memcpy(&crc, fBuf + fLen, 4);
  if (crc != esp_rom_crc32_le(0, fBuf, fLen)) { LINK.printf("N %u\n", fSeq); return; }
  if (fSeq == (uint16_t)(otaSeq - 1)) { LINK.printf("K %u\n", fSeq); return; }   // repeat: our ack was lost
  if (fSeq != otaSeq) { LINK.printf("N %u\n", otaSeq); return; }
  if (!otaWrite(fBuf, fLen)) return;
  otaGot += fLen; otaSeq++;
  LINK.printf("K %u\n", fSeq);
  if (otaGot >= otaExpect) otaFinish();
}

// ESP-NOW firmware server: answers candle pulls straight from the store. Stateless, so any number of
// candles can update at once and a candle can resume from wherever it got to.
// v5.2 2026-10-13: (1) peer slots are recycled least-recently-used - the ESP-NOW table holds 20 incl. broadcast,
// and v4-v5.1 added one per downloading candle and never removed it ("peer leak": the 20th candle's update failed
// silently). (2) At most OTA_MAX_ACTIVE candles download at once (airtime: ~18 KB/s each alone); the rest get
// "busy" = an INFO reply with size 0. Candles 4.11+ don't count busy as a try and ask again ~1 min later.
// Deploy candles 4.11 BEFORE this hub: a 4.10 candle counts "busy" as a failed try.
#define PEER_SLOTS      19              // ESP-NOW max 20 peers, 1 used by broadcast
#define OTA_MAX_ACTIVE  9               // 2026-10-13
#define OTA_IDLE_MS     5000            // a downloader silent this long no longer counts as active
struct PeerSlot { uint8_t mac[6]; uint32_t used; bool on; };
static PeerSlot peers[PEER_SLOTS];
struct Active { uint8_t mac[6]; uint32_t last; bool on; };
static Active act[OTA_MAX_ACTIVE];
static void addPeer(const uint8_t *mac) {           // add or refresh; evict the least recently used slot when full
  uint32_t now = millis(); int freeI = -1, lru = 0;
  for (int i = 0; i < PEER_SLOTS; i++) {
    if (peers[i].on && !memcmp(peers[i].mac, mac, 6)) { peers[i].used = now; return; }
    if (!peers[i].on) { if (freeI < 0) freeI = i; }
    else if (now - peers[i].used > now - peers[lru].used || !peers[lru].on) lru = i;
  }
  int i = freeI;
  if (i < 0) { i = lru; esp_now_del_peer(peers[i].mac); peers[i].on = false;
    Serial.printf("peer slot recycled: %02X%02X%02X\n", peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]); }
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t p = {}; memcpy(p.peer_addr, mac, 6); p.channel = CP_CHANNEL; p.ifidx = WIFI_IF_STA;
    esp_err_t e = esp_now_add_peer(&p);
    if (e != ESP_OK) { Serial.printf("add_peer failed: %s\n", esp_err_to_name(e)); return; }
  }
  memcpy(peers[i].mac, mac, 6); peers[i].used = now; peers[i].on = true;
}
static bool otaAdmit(const uint8_t *mac) {          // true = may download now (already active, or a free place)
  uint32_t now = millis(); int freeI = -1;
  for (int i = 0; i < OTA_MAX_ACTIVE; i++) {
    if (act[i].on && now - act[i].last > OTA_IDLE_MS) act[i].on = false;          // finished or gave up
    if (act[i].on && !memcmp(act[i].mac, mac, 6)) { act[i].last = now; return true; }
    if (!act[i].on && freeI < 0) freeI = i;
  }
  if (freeI < 0) return false;
  memcpy(act[freeI].mac, mac, 6); act[freeI].last = now; act[freeI].on = true;
  Serial.printf("download start %02X%02X%02X\n", mac[3], mac[4], mac[5]);
  return true;
}
static void sendRetry(const uint8_t *mac, const void *d, size_t n) {
  for (int i = 0; i < 20; i++) { if (esp_now_send(mac, (const uint8_t *)d, n) == ESP_OK) return; delay(2); }
}
static void otaServe() {
  OtaRq q;
  while (xQueueReceive(otaQ, &q, 0) == pdTRUE) {
    if (otaMode || !cmeta.magic || q.r.fwId != cmeta.fwId) continue;   // not what we hold, or mid-upload
    addPeer(q.mac);
    if (!otaAdmit(q.mac)) {                          // v5.2: full -> "busy" (INFO with size 0)
      if (q.r.cnt == 0) { CpOtaInfo b = {CP_MAGIC_OTAINFO, CP_VERSION, cmeta.fwId, 0, {0}}; sendRetry(q.mac, &b, sizeof(b)); }
      continue;
    }
    if (q.r.cnt == 0) {
      CpOtaInfo i = {CP_MAGIC_OTAINFO, CP_VERSION, cmeta.fwId, cmeta.size, {0}}; memcpy(i.md5, cmeta.md5, 16);
      sendRetry(q.mac, &i, sizeof(i)); continue;
    }
    CpOtaData dt; dt.magic = CP_MAGIC_OTADATA; dt.ver = CP_VERSION; dt.fwId = cmeta.fwId;
    for (uint8_t k = 0; k < q.r.cnt && k < 16; k++) {
      uint32_t off = q.r.off + (uint32_t)k * CP_OTA_CHUNK; if (off >= cmeta.size) break;
      dt.off = off; dt.len = (uint8_t)min((uint32_t)CP_OTA_CHUNK, cmeta.size - off);
      esp_partition_read(cstg, off, dt.d, dt.len);
      sendRetry(q.mac, &dt, 11 + dt.len);
    }
  }
}

static void otaByte(uint8_t c) {
  uint32_t now = millis();
  if (fState && now - fByteT > 300) fState = 0;    // drop a stale partial frame
  fByteT = otaLastByte = now;
  switch (fState) {
    case 0: if (c == 0xA5) fState = 1; return;
    case 1: fSeq = c; fState = 2; return;
    case 2: fSeq |= (uint16_t)c << 8; fState = 3; return;
    case 3: fLen = c; fState = 4; return;
    case 4: fLen |= (uint16_t)c << 8; fPos = 0; fState = (fLen && fLen <= 1024) ? 5 : 0; return;
    case 5: fBuf[fPos++] = c; if (fPos == fLen + 4) { fState = 0; otaFrame(); } return;
  }
}

static void handleLine(char *l) {
  char *tok = strtok(l, " ");
  if (!tok) return;
  if (!strcmp(tok, "OTAB")) {                       // OTAB <size> <md5>
    char *sz = strtok(NULL, " "), *md5 = strtok(NULL, " ");
    if (!sz || !md5) { LINK.println("OTAERR args"); return; }
    uint32_t n = strtoul(sz, NULL, 10);
    if (!Update.begin(n)) { LINK.printf("OTAERR begin %s\n", Update.errorString()); return; }
    if (!Update.setMD5(md5)) { Update.abort(); LINK.println("OTAERR md5"); return; }
    otaKind = 0; otaMode = true; otaExpect = n; otaGot = 0; otaSeq = 0; fState = 0; otaLastByte = millis();
    Serial.printf("OTA begin %lu bytes\n", (unsigned long)n);
    LINK.println("OTAOK");
    return;
  }
  if (!strcmp(tok, "OTAC")) {                       // OTAC <size> <md5>: candle image to store and serve (v4)
    char *sz = strtok(NULL, " "), *md5 = strtok(NULL, " ");
    if (!sz || !md5 || strlen(md5) != 32) { LINK.println("OTAERR args"); return; }
    if (!cstg) { LINK.println("OTAERR no store partition"); return; }
    uint32_t n = strtoul(sz, NULL, 10);
    if (!n || n > cMetaOff()) { LINK.println("OTAERR size"); return; }
    st.fwId = 0; st.seq++; memset(&cmeta, 0, sizeof(cmeta));          // stop serving the old image right away
    esp_partition_erase_range(cstg, cMetaOff(), 4096);
    strcpy(cMd5Want, md5); cErased = 0;
    otaKind = 1; otaMode = true; otaExpect = n; otaGot = 0; otaSeq = 0; fState = 0; otaLastByte = millis();
    Serial.printf("candle image begin %lu bytes\n", (unsigned long)n);
    LINK.println("OTAOK");
    return;
  }
  if (!strcmp(tok, "PING")) { LINK.printf("PONG %u %u %lu\n", st.n, st.seq, (unsigned long)timerLeftS()); return; }
  if (!strcmp(tok, "DEPTH")) {                       // group flicker depth 0-100
    char *v = strtok(NULL, " "); if (!v) return;
    uint8_t dp = (uint8_t)constrain(atoi(v), 0, 100);
    if (dp != st.depth) { st.depth = dp; st.seq++; }
    Serial.printf("DEPTH %u -> seq %u\n", st.depth, st.seq);
    return;
  }
  if (strcmp(tok, "CMD")) return;
  char *ids = strtok(NULL, " "), *f = strtok(NULL, " "), *w = strtok(NULL, " "), *b = strtok(NULL, " ");
  if (!ids || !f || !w || !b) return;
  uint8_t fl = (uint8_t)atoi(f) & 0x07, wb = (uint8_t)constrain(atoi(w), 1, 255), bb = (uint8_t)constrain(atoi(b), 1, 255);
  if (!strcmp(ids, "ALL")) { for (int i = 0; i < st.n; i++) setEntry(st.e[i].id, fl, wb, bb); }
  else { uint8_t id[3]; if (parseId(ids, id)) setEntry(id, fl, wb, bb); }
  if (!(fl & CP_F_ON) && !strcmp(ids, "ALL")) tmrEnd = 0;     // HA "all off" cancels a running timer
  Serial.printf("CMD %s %u %u %u -> seq %u\n", ids, fl, wb, bb, st.seq);
}

static void handleReport(const Rpt &x) {
  const CpReport &r = x.r;
  if (dupReport(r)) return;
  char fw[24] = "-";                                // v5.2: 8th field "<ver>/<fwId hex>", "-" = old candle
  if (r.fwId) {
    char v[9] = {0}; memcpy(v, r.fw, 8);
    for (char *p = v; *p; p++) if (*p <= ' ' || *p > '~') *p = '?';   // keep the UART line one token
    snprintf(fw, sizeof(fw), "%s/%08lX", v[0] ? v : "?", (unsigned long)r.fwId);
  }
  LINK.printf("RPT %02X%02X%02X %u %u %u %u %d %u %s\n", r.id[0], r.id[1], r.id[2],
              r.flags, r.wbri, r.bbri, r.mv, x.rssi, r.reason, fw);
  Serial.printf("RPT %02X%02X%02X flags=%u wb=%u bb=%u mv=%u rssi=%d why=%u tmr=%uh\n", r.id[0], r.id[1],
                r.id[2], r.flags, r.wbri, r.bbri, r.mv, x.rssi, r.reason, r.tmrH);
  if (findEntry(r.id) < 0) setEntry(r.id, r.flags, r.wbri, r.bbri);   // new candle: adopt its state
  if (r.reason != CP_R_IR) return;
  setAll(r.flags, r.wbri, r.bbri, CP_R_IR);                          // remote on any candle -> every candle
  if (r.tmrH) { tmrEnd = millis() + (uint32_t)r.tmrH * 3600000UL; if (!tmrEnd) tmrEnd = 1;
                Serial.printf("timer %uh started\n", r.tmrH); }
  else if (!(r.flags & CP_F_ON) && tmrEnd) { tmrEnd = 0; Serial.println("timer cancelled (remote OFF)"); }
}

static void timerFire() {
  tmrEnd = 0;
  for (int i = 0; i < st.n; i++) setEntry(st.e[i].id, st.e[i].flags & ~CP_F_ON, st.e[i].wbri, st.e[i].bbri);
  LINK.printf("ALLOFF %u\n", CP_R_TIMER);                           // A: clear on-bits, keep brightness/mode
  Serial.printf("timer fired: all off, seq %u\n", st.seq);
}

void setup() {
  Serial.begin(115200);
  LINK.setRxBufferSize(4096);                      // holds a full 1 KB frame while flash writes block
  LINK.begin(115200, SERIAL_8N1, 16, 17);
  Serial.printf("fw %s\n", FW_TAG);
  rptQ = xQueueCreate(32, sizeof(Rpt));
  otaQ = xQueueCreate(16, sizeof(OtaRq));
  memset(&st, 0, sizeof(st)); st.magic = CP_MAGIC_STATE; st.ver = CP_VERSION; st.depth = CP_DEPTH_DEFAULT;
  cstoreLoad();                                    // resume announcing a stored candle image after a reboot

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  cpEnableLR();                                    // v5: 11b/g/n + LR
  esp_wifi_set_channel(CP_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) { Serial.println("esp_now_init FAILED"); delay(2000); ESP.restart(); }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(cpOnSent);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BCAST, 6); p.channel = CP_CHANNEL; p.ifidx = WIFI_IF_STA; p.encrypt = false;
  esp_now_add_peer(&p);

  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  Serial.printf("hub B v5.2 (LR, LRU peers, max %d downloads) up, mac %s, channel %d\n", OTA_MAX_ACTIVE, macStr, CP_CHANNEL);
  LINK.printf("HELLO %s\n", macStr);
}

void loop() {
  static uint32_t lastTx = 0, lastHello = 0;
  static char buf[64]; static uint8_t len = 0;

  if (otaMode && millis() - otaLastByte > 10000) otaAbort("timeout");
  while (LINK.available()) {                       // lines from board A
    char c = LINK.read();
    if (otaMode) { otaByte((uint8_t)c); continue; }   // firmware frames, not text
    if (c == '\n' || c == '\r') { if (len) { buf[len] = 0; handleLine(buf); len = 0; } }
    else if (len < sizeof(buf) - 1) buf[len++] = c;
  }

  Rpt x;                                           // candle reports
  while (xQueueReceive(rptQ, &x, 0) == pdTRUE) handleReport(x);
  otaServe();                                      // candle firmware pulls (v4)

  uint32_t now = millis();
  if (tmrEnd && (int32_t)(now - tmrEnd) >= 0) timerFire();
  if (now - lastTx >= 50) {                        // continuous rebroadcast (candles listen 80 ms)
    lastTx = now;
    cpSendBoth(BCAST, &st, CP_STATE_LEN(st.n));    // v5: normal rate (v4 candles) + LR (far v4.6 candles)
  }
  if (st.n == 0 && now - lastHello >= 10000) {      // A may have booted after us
    lastHello = now; LINK.printf("HELLO %s\n", macStr);
  }
}
