// hub_a.ino - Candle hub, board A: WiFi + MQTT (Home Assistant discovery) + UART to board B.
// v2 2026-10-11: two HA lights per candle (Wick with Flicker/Steady effect, Base); remote/timer
// group changes come from B as ALL / ALLOFF lines. UART is serviced even while WiFi is down.
// Board: ESP32-DevKitC-32E (FQBN esp32:esp32:esp32)
// UART to board B: RX = GPIO16 (from B GPIO17), TX = GPIO17 (to B GPIO16), 115200 8N1
//
// MQTT topics (base "candles"):
//   candles/hub/status            online/offline (LWT, retained)
//   candles/<id>/set|state        wick  {"state":"ON","brightness":180,"effect":"Flicker"}
//   candles/<id>/base/set|state   base  {"state":"ON","brightness":120}
//   candles/<id>/attr             {"vbat":..,"rssi":..,"last_seen":".."}, retained
//   candles/<id>/avail            online/offline, retained (offline after 3 days silent)
//   candles/all/set               group light: state -> wick+base, brightness/effect -> wick
//   candles/flicker/set|state     v3 group "Candle flicker": ON = flicker, OFF = steady, brightness = depth (HomeKit)
//
// v5 2026-10-12: no settings compiled in. WiFi / MQTT / OTA password / hub name / topic prefix live in NVS
// (hub_config.h) and are set from the hub's setup page:
//   - not configured, or WiFi down 5 min, or BOOT held 3 s -> hub opens network "CandleHub-XXXX" (WPA2, password
//     on serial and on the /config page); join it and the setup page opens (captive portal) at http://192.168.4.1/
//   - on the LAN: http://<hub>/config  (user admin, OTA password)
//   - BOOT held 10 s -> erase all settings, reboot into setup
// "candles" above = the configured topic prefix (cfg.base). Default for the migrated hub stays "candles".
#include <WiFi.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <WebServer.h>
#include <MD5Builder.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <Update.h>
#include "hub_config.h"                             // v5: settings in NVS (secrets.h only seeds them once, if present)
const char FW_TAG[] = "@@FWTAG:hub_a@@";        // image tag: the relay refuses images without the right tag

#define LINK Serial2
#define MAX_NODES 40
#define OFFLINE_AFTER_S (3UL * 24 * 3600 + 3600)
#define F_WICK   0x01
#define F_BASE   0x02
#define F_STEADY 0x04
#define F_ON     (F_WICK | F_BASE)

WiFiClient net;
PubSubClient mq(net);

struct Node {
  bool used, known, announced, online;
  char id[7];
  uint8_t flags, wbri, bbri;
  uint16_t mv; int8_t rssi;
  time_t seen;
  char fw[24];                                     // v5.2: "<ver>/<fwId hex>" from the candle's report, "" = unknown
};
Node nodes[MAX_NODES];

// v3: group "Candle flicker" light - on = flicker, off = steady, brightness = depth (HomeKit can't see effects)
#define DEPTH_DEFAULT 72
bool gFlick = true, gSeeded = false;
uint8_t gDepth = DEPTH_DEFAULT;                    // 0-100
uint32_t gMqttUpAt = 0;

Node *findNode(const char *id, bool add) {
  for (auto &n : nodes) if (n.used && !strcmp(n.id, id)) return &n;
  if (!add) return nullptr;
  for (auto &n : nodes) if (!n.used) { memset(&n, 0, sizeof(n)); n.used = true; strncpy(n.id, id, 6); n.wbri = 255; n.bbri = 255; return &n; }
  return nullptr;
}

bool validId(const char *s) {
  if (strlen(s) != 6) return false;
  for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) return false;
  return true;
}

void topic(char *out, size_t sz, const char *id, const char *leaf) { snprintf(out, sz, "%s/%s/%s", cfg.base, id, leaf); }
char T_STATUS[40], T_ALLSET[40], T_FLKSET[40], T_FLKSTATE[40], HUB_UID[40];   // v5: built from cfg.base in setup()
void buildTopics() {
  snprintf(T_STATUS, sizeof(T_STATUS), "%s/hub/status", cfg.base);
  snprintf(T_ALLSET, sizeof(T_ALLSET), "%s/all/set", cfg.base);
  snprintf(T_FLKSET, sizeof(T_FLKSET), "%s/flicker/set", cfg.base);
  snprintf(T_FLKSTATE, sizeof(T_FLKSTATE), "%s/flicker/state", cfg.base);
  hubUid(HUB_UID, sizeof(HUB_UID));
}

void pubJson(const char *t, JsonDocument &doc, bool retain) {
  if (!mq.connected()) return;
  char buf[900]; size_t n = serializeJson(doc, buf, sizeof(buf));
  mq.publish(t, (const uint8_t *)buf, n, retain);
}

bool linkBusy();                                   // true while a firmware image is streaming to B
void sendCmd(Node &n) { if (linkBusy()) return; LINK.printf("CMD %s %u %u %u\n", n.id, n.flags, n.wbri, n.bbri); }

void pubState(Node &n) {
  char t[48];
  JsonDocument w; w["state"] = (n.flags & F_WICK) ? "ON" : "OFF"; w["brightness"] = n.wbri;
  w["effect"] = (n.flags & F_STEADY) ? "Steady" : "Flicker";
  topic(t, sizeof(t), n.id, "state"); pubJson(t, w, true);
  JsonDocument b; b["state"] = (n.flags & F_BASE) ? "ON" : "OFF"; b["brightness"] = n.bbri;
  topic(t, sizeof(t), n.id, "base/state"); pubJson(t, b, true);
}

void pubFlicker() {
  JsonDocument d; d["state"] = gFlick ? "ON" : "OFF"; d["brightness"] = (gDepth * 255 + 50) / 100;
  pubJson(T_FLKSTATE, d, true);
}

void sendDepth() { if (linkBusy()) return; LINK.printf("DEPTH %u\n", gDepth); }

// Flicker on/off for every candle: clear/set the steady bit, keep each candle's on/brightness.
void setAllFlicker(bool on) {
  gFlick = on;
  for (auto &n : nodes) if (n.used && n.known) {
    uint8_t f = on ? (n.flags & ~F_STEADY) : (n.flags | F_STEADY);
    if (f != n.flags) { n.flags = f; sendCmd(n); pubState(n); }
  }
}

void isoTime(time_t t, char *out, size_t sz) { struct tm g; gmtime_r(&t, &g); strftime(out, sz, "%Y-%m-%dT%H:%M:%SZ", &g); }

void pubAttr(Node &n) {
  JsonDocument d;
  if (n.mv) d["vbat"] = n.mv / 1000.0; else d["vbat"] = nullptr;
  d["rssi"] = n.rssi;
  char ts[24]; isoTime(n.seen, ts, sizeof(ts)); d["last_seen"] = ts;
  if (n.fw[0]) {                                   // v5.2: "4.11/00CF1A5C" -> fw "4.11", fw_id "00CF1A5C"
    char v[24]; strlcpy(v, n.fw, sizeof(v)); char *sl = strchr(v, '/');
    if (sl) { *sl = 0; d["fw_id"] = sl + 1; } else d["fw_id"] = nullptr;
    d["fw"] = v;
  } else { d["fw"] = nullptr; d["fw_id"] = nullptr; }
  char t[48]; topic(t, sizeof(t), n.id, "attr"); pubJson(t, d, true);
}

void pubAvail(Node &n, bool online) {
  n.online = online;
  if (!mq.connected()) return;
  char t[48]; topic(t, sizeof(t), n.id, "avail"); mq.publish(t, online ? "online" : "offline", true);
}

void addDevice(JsonDocument &d, const char *id) {
  JsonObject dev = d["dev"].to<JsonObject>();
  char ids[20]; snprintf(ids, sizeof(ids), "candle_%s", id);
  dev["ids"].to<JsonArray>().add(ids);
  char nm[20]; snprintf(nm, sizeof(nm), "Candle %s", id); dev["name"] = nm;
  dev["mdl"] = "ESP-NOW candle (C3)"; dev["mf"] = "DIY"; dev["via_device"] = HUB_UID;
  Node *n = findNode(id, false);                   // v5.2: firmware on the HA device page (re-announced on change)
  if (n && n->fw[0]) { char v[24]; strlcpy(v, n->fw, sizeof(v)); char *sl = strchr(v, '/'); if (sl) *sl = 0; dev["sw"] = v; }
}

void addAvail(JsonDocument &d, const char *id) {
  JsonArray a = d["avty"].to<JsonArray>();
  a.add<JsonObject>()["t"] = T_STATUS;
  char t[48]; topic(t, sizeof(t), id, "avail"); a.add<JsonObject>()["t"] = t;
  d["avty_mode"] = "all";
}

void announceSensor(const char *id, const char *key, const char *name, const char *cls,
                    const char *unit, const char *tpl, bool diag) {
  JsonDocument d; char uid[40], t[48], ct[80];
  snprintf(uid, sizeof(uid), "candle_%s_%s", id, key);
  d["name"] = name; d["uniq_id"] = uid; if (cls) d["dev_cla"] = cls;   // v5.2: text sensor (fw) has no class
  if (unit) { d["unit_of_meas"] = unit; d["stat_cla"] = "measurement"; }
  if (diag) d["ent_cat"] = "diagnostic";
  topic(t, sizeof(t), id, "attr"); d["stat_t"] = t; d["val_tpl"] = tpl;
  addAvail(d, id); addDevice(d, id);
  snprintf(ct, sizeof(ct), "homeassistant/sensor/candle_%s_%s/config", id, key);
  pubJson(ct, d, true);
}

// v5.1 2026-10-13: low-battery flag per candle (binary_sensor, device_class battery) so generic battery reports
// (n8n weekly) see candles as ok/low. Volts stay the measurement; the flag is a cutoff on the corrected reading
// (candle 4.7 reports true volts). NiMH 2 cells: ~1.1 V/cell knee -> 2.20 V. vbat null/0 (sense not wired) = ok.
#define BATLOW_V "2.20"
void announceBatteryLow(const char *id) {
  JsonDocument d; char uid[40], t[48], ct[96];
  snprintf(uid, sizeof(uid), "candle_%s_battery_low", id);
  d["name"] = "Battery low"; d["uniq_id"] = uid; d["obj_id"] = uid; d["dev_cla"] = "battery";
  topic(t, sizeof(t), id, "attr"); d["stat_t"] = t;
  d["val_tpl"] = "{{ 'ON' if (value_json.vbat is number and value_json.vbat > 0 and value_json.vbat < " BATLOW_V ") else 'OFF' }}";
  addAvail(d, id); addDevice(d, id);
  snprintf(ct, sizeof(ct), "homeassistant/binary_sensor/%s/config", uid);
  pubJson(ct, d, true);
}

// One light entity. uid suffix "" = wick (keeps the v1 unique_id so the HA entity survives), "_base" = base.
void announceLight(const char *id, const char *sfx, const char *name, const char *leafSet,
                   const char *leafState, bool effects) {
  JsonDocument d; char uid[32], t1[48], t2[48], ct[80];
  snprintf(uid, sizeof(uid), "candle_%s%s", id, sfx);
  d["name"] = name; d["uniq_id"] = uid; d["schema"] = "json"; d["brightness"] = true;
  if (effects) { d["effect"] = true; JsonArray fx = d["fx_list"].to<JsonArray>(); fx.add("Flicker"); fx.add("Steady"); }
  topic(t1, sizeof(t1), id, leafSet); d["cmd_t"] = t1;
  topic(t2, sizeof(t2), id, leafState); d["stat_t"] = t2;
  addAvail(d, id); addDevice(d, id);
  snprintf(ct, sizeof(ct), "homeassistant/light/%s/config", uid);
  pubJson(ct, d, true);
}

void announce(Node &n) {
  if (!mq.connected()) return;                    // retried for every node on MQTT (re)connect
  announceLight(n.id, "", "Wick", "set", "state", true);
  announceLight(n.id, "_base", "Base", "base/set", "base/state", false);
  announceSensor(n.id, "vbat", "Battery", "voltage", "V", "{{ value_json.vbat }}", false);
  announceBatteryLow(n.id);                        // v5.1
  announceSensor(n.id, "rssi", "Signal", "signal_strength", "dBm", "{{ value_json.rssi }}", true);
  announceSensor(n.id, "seen", "Last seen", "timestamp", nullptr, "{{ value_json.last_seen }}", true);
  announceSensor(n.id, "fw", "Firmware", nullptr, nullptr,                        // v5.2
                 "{{ value_json.fw ~ ' (' ~ value_json.fw_id ~ ')' if value_json.fw else 'unknown (pre-4.11)' }}", true);
  n.announced = true;
  Serial.printf("announced candle %s\n", n.id);
}

void announceHub() {                               // v5: ids/topics from cfg; prefix "candles" gives the v1-v4 ids
  char uid[48], ct[96];
  JsonDocument d;
  snprintf(uid, sizeof(uid), "%s_all", HUB_UID);
  d["name"] = "All candles"; d["uniq_id"] = uid; d["schema"] = "json";
  d["brightness"] = true; d["effect"] = true; d["opt"] = true; d["cmd_t"] = T_ALLSET;
  JsonArray fx = d["fx_list"].to<JsonArray>(); fx.add("Flicker"); fx.add("Steady");
  d["avty_t"] = T_STATUS;
  JsonObject dev = d["dev"].to<JsonObject>();
  dev["ids"].to<JsonArray>().add(HUB_UID); dev["name"] = hubDisplayName();
  dev["mdl"] = "2x ESP32-DevKitC (WiFi + ESP-NOW)"; dev["mf"] = "DIY";
  snprintf(ct, sizeof(ct), "homeassistant/light/%s/config", uid);
  pubJson(ct, d, true);

  JsonDocument f;                                  // group flicker: on = flicker, brightness = depth
  snprintf(uid, sizeof(uid), "%s_flicker", HUB_UID);
  f["name"] = "Candle flicker"; f["uniq_id"] = uid; f["schema"] = "json";
  f["brightness"] = true; f["cmd_t"] = T_FLKSET; f["stat_t"] = T_FLKSTATE;
  f["avty_t"] = T_STATUS; f["icon"] = "mdi:candle";
  JsonObject fd = f["dev"].to<JsonObject>(); fd["ids"].to<JsonArray>().add(HUB_UID);
  snprintf(ct, sizeof(ct), "homeassistant/light/%s/config", uid);
  pubJson(ct, f, true);
}

// Apply an HA light payload to one channel. bit = F_WICK or F_BASE. Returns true if anything changed.
bool applySet(Node &n, JsonDocument &d, uint8_t bit) {
  uint8_t f = n.flags, &bri = (bit == F_WICK) ? n.wbri : n.bbri, b = bri;
  if (d["state"].is<const char *>()) { if (!strcmp(d["state"], "ON")) f |= bit; else f &= ~bit; }
  if (d["brightness"].is<int>()) { int v = constrain(d["brightness"].as<int>(), 0, 255); if (v == 0) f &= ~bit; else b = v; }
  if (bit == F_WICK && d["effect"].is<const char *>()) { if (!strcmp(d["effect"], "Steady")) f |= F_STEADY; else f &= ~F_STEADY; }
  bool changed = !n.known || f != n.flags || b != bri;
  n.flags = f; bri = b; n.known = true;
  return changed;
}

void onMqtt(char *t, byte *p, unsigned int len) {
  // t = <base>/<id>/<leaf>, leaf = set | state | base/set | base/state
  char id[16] = {0}, leaf[16] = {0};
  size_t bl = strlen(cfg.base);
  if (strncmp(t, cfg.base, bl) || t[bl] != '/') return;
  if (sscanf(t + bl + 1, "%15[^/]/%15s", id, leaf) != 2) return;
  JsonDocument d;
  if (deserializeJson(d, p, len)) return;
  if (!strcmp(id, "all") && !strcmp(leaf, "set")) {           // group: state -> both LEDs, rest -> wick
    for (auto &n : nodes) if (n.used) {
      JsonDocument c; c.set(d); bool ch = applySet(n, c, F_WICK);
      if (d["state"].is<const char *>()) { JsonDocument s; s["state"] = d["state"]; ch |= applySet(n, s, F_BASE); }
      if (ch) { sendCmd(n); pubState(n); }
    }
    if (d["effect"].is<const char *>()) { gFlick = strcmp(d["effect"], "Steady") != 0; pubFlicker(); }
    Serial.println("ALL set from HA");
    return;
  }
  if (!strcmp(id, "flicker")) {
    if (!strcmp(leaf, "state")) {                               // retained: restore depth + mode after reboot
      if (gSeeded) return;
      if (d["brightness"].is<int>()) gDepth = constrain((d["brightness"].as<int>() * 100 + 127) / 255, 1, 100);
      if (d["state"].is<const char *>()) gFlick = !strcmp(d["state"], "ON");
      gSeeded = true; sendDepth();
      Serial.printf("flicker seeded on=%d depth=%u\n", gFlick, gDepth);
      return;
    }
    if (strcmp(leaf, "set")) return;
    gSeeded = true;
    bool on = gFlick;
    if (d["state"].is<const char *>()) on = !strcmp(d["state"], "ON");
    if (d["brightness"].is<int>()) {
      int v = constrain(d["brightness"].as<int>(), 0, 255);
      if (v == 0) on = false; else { uint8_t dp = constrain((v * 100 + 127) / 255, 1, 100); if (dp != gDepth) { gDepth = dp; sendDepth(); } }
    }
    if (on != gFlick) setAllFlicker(on);
    pubFlicker();
    Serial.printf("FLICKER on=%d depth=%u\n", gFlick, gDepth);
    return;
  }
  if (!validId(id)) return;
  bool base = !strncmp(leaf, "base/", 5);
  const char *l = base ? leaf + 5 : leaf;
  uint8_t bit = base ? F_BASE : F_WICK;
  if (!strcmp(l, "set")) {
    Node *n = findNode(id, true); if (!n) return;
    if (applySet(*n, d, bit)) sendCmd(*n);
    pubState(*n);
    Serial.printf("SET %s %s flags=%u wb=%u bb=%u\n", id, base ? "base" : "wick", n->flags, n->wbri, n->bbri);
  } else if (!strcmp(l, "state")) {                           // retained state seeds the table after a reboot
    Node *n = findNode(id, true); if (!n || n->seen) return;   // a live report beats retained state
    bool wasKnown = n->known; applySet(*n, d, bit); n->known = true; sendCmd(*n);
    if (!wasKnown) Serial.printf("seeded %s\n", id);
  }
}

// ---- v3.1 firmware relay: PC -> (HTTP upload) -> A staging flash -> (UART frames) -> B. B never joins WiFi. ----
// Upload:  curl -u admin:<OTA_PASS> -F "fw=@hub_b.ino.bin" http://candle-hub-a.local/update-b   then poll /status
// Frame A->B: A5 seqLo seqHi lenLo lenHi data[len<=1024] crc32(data, LE). B answers "K <seq>" / "N <seq>".
WebServer web(80);
DNSServer dns;
bool portalOn = false;                              // v5: hub's own setup network is up
uint32_t portalAt = 0;

// v5: request arrived over the hub's own setup network (WPA2 already gates it) -> no HTTP password needed.
bool viaPortal() { return portalOn && web.client().localIP() == WiFi.softAPIP(); }
bool uploadAuthOk() { return cfg.otapass[0] && web.authenticate("admin", cfg.otapass); }
bool configAuthOk() { return viaPortal() || uploadAuthOk(); }
const esp_partition_t *stg = nullptr;            // staging = the unused SPIFFS data partition
uint32_t stgLen = 0, stgErased = 0;
MD5Builder upMd5; char stgMd5[33] = "";
bool upReject = false, upTagOk = false, upTooBig = false;
uint8_t upKind = 0;                                // 0 = /update-b, 1 = /update-candle (stored on B, pulled by candles), 2 = /update-a (A itself)
uint8_t upCarry[32]; uint8_t upCarryN = 0;
enum XState { XS_IDLE, XS_BEGIN, XS_SEND, XS_END, XS_OK, XS_FAIL };
XState xs = XS_IDLE; uint32_t xOff = 0, xT = 0; uint16_t xSeq = 0, xLen = 0; uint8_t xTries = 0;
char xMsg[128] = "idle";
#define XCHUNK 1024
uint8_t xBuf[XCHUNK];

bool linkBusy() { return xs == XS_BEGIN || xs == XS_SEND || xs == XS_END; }

// Look for the image tag, including across chunk boundaries (keeps the last 31 bytes).
bool tagScan(const uint8_t *b, size_t n, const char *needle) {
  size_t nl = strlen(needle); bool hit = false;
  uint8_t t[96]; size_t c = upCarryN, k = min(n, (size_t)31);
  memcpy(t, upCarry, c); memcpy(t + c, b, k);
  for (size_t i = 0; i + nl <= c + k && !hit; i++) if (!memcmp(t + i, needle, nl)) hit = true;
  for (size_t i = 0; i + nl <= n && !hit; i++) if (b[i] == '@' && !memcmp(b + i, needle, nl)) hit = true;
  if (n >= 31) { memcpy(upCarry, b + n - 31, 31); upCarryN = 31; }
  else { memcpy(t, upCarry, c); memcpy(t + c, b, n); size_t tot = c + n, kk = min(tot, (size_t)31); memcpy(upCarry, t + tot - kk, kk); upCarryN = kk; }
  return hit;
}

void onUpload() {
  HTTPUpload &u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    upKind = (web.uri() == "/update-candle") ? 1 : (web.uri() == "/update-a") ? 2 : 0;
    upReject = !uploadAuthOk() || linkBusy();
    upTagOk = upTooBig = false; stgLen = stgErased = 0; upCarryN = 0; upMd5.begin();
    if (!stg) stg = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (!stg) upReject = true;
    if (!upReject && upKind == 2 && !Update.begin(UPDATE_SIZE_UNKNOWN)) upReject = true;   // A writes its own spare slot
    Serial.printf("upload start %s kind=%u reject=%d\n", u.filename.c_str(), upKind, upReject);
  } else if (u.status == UPLOAD_FILE_WRITE && !upReject && !upTooBig && upKind == 2) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) upTooBig = true;
    if (!upTagOk && tagScan(u.buf, u.currentSize, "@@FWTAG:hub_a@@")) upTagOk = true;
    stgLen += u.currentSize;
  } else if (u.status == UPLOAD_FILE_WRITE && !upReject && !upTooBig) {
    if (stgLen + u.currentSize > min((uint32_t)stg->size, (uint32_t)0x140000)) { upTooBig = true; return; }
    while (stgErased < stgLen + u.currentSize) { esp_partition_erase_range(stg, stgErased, 4096); stgErased += 4096; }
    esp_partition_write(stg, stgLen, u.buf, u.currentSize);
    upMd5.add(u.buf, u.currentSize);
    if (!upTagOk && tagScan(u.buf, u.currentSize, upKind ? "@@FWTAG:candle@@" : "@@FWTAG:hub_b@@")) upTagOk = true;
    stgLen += u.currentSize;
  } else if (u.status == UPLOAD_FILE_END && !upReject) {
    upMd5.calculate(); strncpy(stgMd5, upMd5.toString().c_str(), 32); stgMd5[32] = 0;
    Serial.printf("upload end %lu bytes md5 %s tag=%d\n", (unsigned long)stgLen, stgMd5, upTagOk);
  }
}

void sendFrame() {
  xLen = min((uint32_t)XCHUNK, stgLen - xOff);
  esp_partition_read(stg, xOff, xBuf, xLen);
  uint32_t crc = esp_rom_crc32_le(0, xBuf, xLen);
  uint8_t h[5] = {0xA5, (uint8_t)xSeq, (uint8_t)(xSeq >> 8), (uint8_t)xLen, (uint8_t)(xLen >> 8)};
  LINK.write(h, 5); LINK.write(xBuf, xLen); LINK.write((uint8_t *)&crc, 4);
  xT = millis();
  snprintf(xMsg, sizeof(xMsg), "sending %lu/%lu", (unsigned long)xOff, (unsigned long)stgLen);
}

void xferFail(const char *why) {
  xs = XS_FAIL; snprintf(xMsg, sizeof(xMsg), "FAIL %s at %lu/%lu", why, (unsigned long)xOff, (unsigned long)stgLen);
  Serial.println(xMsg);
  delay(50); for (auto &n : nodes) if (n.used && n.known) sendCmd(n);   // B kept old firmware: resync it
  sendDepth();
}

void onUploadDone() {
  if (upReject) { if (!uploadAuthOk()) return web.requestAuthentication();
                  web.send(409, "text/plain", "busy (transfer running) or no staging partition\n"); return; }
  if (upKind == 2) {                               // A itself: only switch to the new image if it is a hub_a image and verifies
    if (upTooBig || !upTagOk) { Update.abort(); web.send(400, "text/plain", upTooBig ? "write failed / too big\n" : "rejected: not a hub_a image (tag @@FWTAG:hub_a@@ missing)\n"); return; }
    if (!Update.end(true)) { web.send(500, "text/plain", String("update failed: ") + Update.errorString() + "\n"); return; }
    web.send(200, "text/plain", "hub A updated - rebooting\n"); delay(500); ESP.restart();
  }
  if (upTooBig) { web.send(413, "text/plain", "image too big\n"); return; }
  if (!upTagOk) { web.send(400, "text/plain", upKind ? "rejected: not a candle image (tag @@FWTAG:candle@@ missing)\n"
                                                       : "rejected: not a hub_b image (tag @@FWTAG:hub_b@@ missing)\n"); return; }
  xs = XS_BEGIN; xOff = 0; xSeq = 0; xTries = 0; xT = millis();
  LINK.printf("\n%s %lu %s\n", upKind ? "OTAC" : "OTAB", (unsigned long)stgLen, stgMd5);
  snprintf(xMsg, sizeof(xMsg), "begin %s %lu bytes", upKind ? "candle image" : "hub B", (unsigned long)stgLen);
  char r[180]; snprintf(r, sizeof(r), "stored %lu bytes md5 %s - sending %s to hub B, poll /status\n", (unsigned long)stgLen, stgMd5,
                        upKind ? "candle image" : "firmware");
  web.send(200, "text/plain", r);
}

// B's replies during a transfer. Returns true if the line was a relay line.
bool otaLine(const char *l) {
  if (!linkBusy()) return false;
  unsigned s;
  if (!strcmp(l, "OTAOK") && xs == XS_BEGIN) { xs = XS_SEND; xTries = 0; sendFrame(); return true; }
  if (!strncmp(l, "OTAERR", 6) || !strncmp(l, "OTAFAIL", 7)) { xferFail(l); return true; }
  if (sscanf(l, "K %u", &s) == 1) {
    if (xs != XS_SEND || s != xSeq) return true;   // stale ack
    xOff += xLen; xSeq++; xTries = 0;
    if (xOff >= stgLen) { xs = XS_END; xT = millis(); snprintf(xMsg, sizeof(xMsg), "all %lu bytes sent, B verifying", (unsigned long)stgLen); }
    else sendFrame();
    return true;
  }
  if (sscanf(l, "N %u", &s) == 1) { if (xs == XS_SEND && s == xSeq) { if (++xTries > 8) xferFail("too many NAKs"); else sendFrame(); } return true; }
  if (!strncmp(l, "OTADONE", 7) && xs == XS_END) {
    xs = XS_OK;
    if (upKind) {                                  // B stays up: give it back any HA changes held during the transfer
      snprintf(xMsg, sizeof(xMsg), "OK candle image on B (%s) - candles update at next check-in", l + 8);
      for (auto &n : nodes) if (n.used && n.known) sendCmd(n);
      sendDepth();
    }
    else snprintf(xMsg, sizeof(xMsg), "OK %lu bytes md5 %s - B rebooting", (unsigned long)stgLen, stgMd5);
    Serial.println(xMsg); return true;            // B's HELLO after reboot triggers the table resync
  }
  return false;
}

void otaTick() {
  if (xs == XS_BEGIN && millis() - xT > 10000) xferFail("no OTAOK from B (old B firmware?)");
  else if (xs == XS_SEND && millis() - xT > 1500) { if (++xTries > 8) xferFail("no ack"); else sendFrame(); }
  else if (xs == XS_END && millis() - xT > 20000) xferFail("no OTADONE");
}

// ---- v5 setup page ----
void portalStart(const char *why) {
  if (portalOn) return;
  char tail[5], ap[24]; macTail(tail, true); snprintf(ap, sizeof(ap), "CandleHub-%s", tail);
  WiFi.mode(WIFI_AP_STA);                          // STA keeps retrying the saved network meanwhile
  WiFi.softAP(ap, cfg.appass);
  dns.start(53, "*", WiFi.softAPIP());             // every name -> the hub = phone shows the setup page
  portalOn = true; portalAt = millis();
  Serial.printf("SETUP NETWORK %s up (%s). Password: %s  Page: http://%s/\n", ap, why, cfg.appass,
                WiFi.softAPIP().toString().c_str());
}
void portalStop() {
  if (!portalOn) return;
  dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); portalOn = false;
  Serial.println("setup network closed");
}

String esc(const char *s) {                        // HTML-escape a value for an attribute
  String o; for (; *s; s++) {
    if (*s == '&') o += "&amp;"; else if (*s == '"') o += "&quot;"; else if (*s == '<') o += "&lt;";
    else if (*s == '>') o += "&gt;"; else o += *s;
  }
  return o;
}

void sendConfigPage(const char *msg) {
  String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Hub setup</title>"
               "<style>body{font:16px system-ui,sans-serif;max-width:30em;margin:1em auto;padding:0 1em}"
               "label{display:block;margin:.7em 0 .2em;font-weight:600}input{width:100%;padding:.5em;box-sizing:border-box}"
               "small{color:#666}.m{padding:.6em;background:#fee;border:1px solid #c88;margin:.6em 0}"
               "button{margin-top:1.2em;padding:.7em 1.4em;font-size:1em}</style><h2>Hub setup</h2>");
  if (msg && *msg) { h += "<div class=m>"; h += msg; h += "</div>"; }
  h += F("<form method=POST action=/config>");
  auto f = [&](const char *label, const char *name, const char *val, const char *type, const char *hint) {
    h += "<label>"; h += label; h += "</label><input name="; h += name; h += " type="; h += type;
    if (!strcmp(type, "password")) { h += " placeholder='"; h += (val && *val) ? "(unchanged)" : "(not set)"; h += "'"; }
    else { h += " value=\""; h += esc(val); h += "\""; }
    h += ">"; if (hint) { h += "<small>"; h += hint; h += "</small>"; }
  };
  char port[8]; snprintf(port, sizeof(port), "%u", cfg.mport);
  f("WiFi network", "ssid", cfg.ssid, "text", nullptr);
  f("WiFi password", "pass", cfg.pass, "password", "Leave blank to keep the current one.");
  f("MQTT host", "mhost", cfg.mhost, "text", "IP or name of the broker (Home Assistant).");
  f("MQTT port", "mport", port, "number", nullptr);
  f("MQTT user", "muser", cfg.muser, "text", nullptr);
  f("MQTT password", "mpass", cfg.mpass, "password", nullptr);
  f("Update (OTA) password", "otapass", cfg.otapass, "password", "Protects firmware uploads and this page on your network (user: admin).");
  f("Hub name", "name", cfg.name, "text", "Unique per hub. Letters, digits, dash. Hostname and MQTT client id.");
  f("MQTT topic prefix", "base", cfg.base, "text", "Unique per hub. Letters, digits, _ or -. The first candle hub uses: candles");
  f("Setup network password", "appass", cfg.appass, "text", "For the CandleHub-XXXX network this page runs on. 8+ characters.");
  h += F("<button>Save and restart</button></form>");
  web.send(200, "text/html", h);
}

bool okChars(const char *s, const char *extra, size_t maxLen) {
  size_t n = strlen(s); if (!n || n > maxLen) return false;
  for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr(extra, *s)) return false;
  return true;
}

void handleConfigPost() {
  if (!configAuthOk()) return web.requestAuthentication();
  HubCfg n = cfg;
  String v;
  v = web.arg("ssid");  strlcpy(n.ssid, v.c_str(), sizeof(n.ssid));
  v = web.arg("pass");  if (v.length()) strlcpy(n.pass, v.c_str(), sizeof(n.pass));
  v = web.arg("mhost"); strlcpy(n.mhost, v.c_str(), sizeof(n.mhost));
  long p = web.arg("mport").toInt(); n.mport = (p > 0 && p < 65536) ? p : 1883;
  v = web.arg("muser"); strlcpy(n.muser, v.c_str(), sizeof(n.muser));
  v = web.arg("mpass"); if (v.length()) strlcpy(n.mpass, v.c_str(), sizeof(n.mpass));
  v = web.arg("otapass"); if (v.length()) strlcpy(n.otapass, v.c_str(), sizeof(n.otapass));
  v = web.arg("name");  v.trim(); strlcpy(n.name, v.c_str(), sizeof(n.name));
  v = web.arg("base");  v.trim(); strlcpy(n.base, v.c_str(), sizeof(n.base));
  v = web.arg("appass"); if (v.length()) strlcpy(n.appass, v.c_str(), sizeof(n.appass));
  const char *err = nullptr;
  if (!n.ssid[0]) err = "WiFi network is required.";
  else if (!n.otapass[0]) err = "Set an update (OTA) password.";
  else if (!okChars(n.name, "-", 31)) err = "Hub name: 1-31 letters, digits or dashes.";
  else if (!okChars(n.base, "_-", 24)) err = "Topic prefix: 1-24 letters, digits, _ or -.";
  else if (strlen(n.appass) < 8) err = "Setup network password needs 8+ characters.";
  if (err) { HubCfg keep = cfg; cfg = n; sendConfigPage(err); cfg = keep; return; }
  cfg = n; cfgSave();
  web.send(200, "text/html", "<meta name=viewport content='width=device-width'><h3>Saved.</h3>"
                             "The hub is restarting and will join your WiFi. You can close this page.");
  Serial.println("cfg: saved from setup page - restarting");
  delay(1000); ESP.restart();
}

void webSetup() {
  web.on("/", HTTP_GET, []() {
    if (viaPortal()) { web.sendHeader("Location", "/config"); web.send(302, "text/plain", ""); return; }
    String h = "<h3>"; h += cfg.name;
    h += "</h3><form method=POST action=/update-b enctype=multipart/form-data>"
         "Hub B firmware (.bin): <input type=file name=fw> <input type=submit value=Send></form>"
         "<form method=POST action=/update-candle enctype=multipart/form-data>"
         "Candle firmware (.bin): <input type=file name=fw> <input type=submit value=Send></form>"
         "<p><a href=/status>status</a> &middot; <a href=/config>settings</a>";
    web.send(200, "text/html", h);
  });
  web.on("/config", HTTP_GET, []() { if (!configAuthOk()) return web.requestAuthentication(); sendConfigPage(nullptr); });
  web.on("/config", HTTP_POST, handleConfigPost);
  web.on("/status", HTTP_GET, []() { char r[140]; snprintf(r, sizeof(r), "%s\n", xMsg); web.send(200, "text/plain", r); });
  web.on("/update-b", HTTP_POST, onUploadDone, onUpload);
  web.on("/update-candle", HTTP_POST, onUploadDone, onUpload);
  web.on("/update-a", HTTP_POST, onUploadDone, onUpload);
  web.onNotFound([]() {                            // captive portal: phones probe random URLs -> send them to setup
    if (portalOn) { web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/config"); web.send(302, "text/plain", ""); }
    else web.send(404, "text/plain", "not found\n");
  });
  web.begin();
  Serial.println("web up: / /config /status /update-*");
}

char lastB[64] = "-";
void handleB(char *l) {
  strncpy(lastB, l, sizeof(lastB) - 1);
  if (otaLine(l)) return;
  if (!strncmp(l, "HELLO", 5)) {                  // B rebooted: give it the whole table
    Serial.printf("B: %s\n", l);
    for (auto &n : nodes) if (n.used && n.known) sendCmd(n);
    sendDepth();
    return;
  }
  unsigned f, wb, bb, mv, why; int rssi; char id[8];
  if (sscanf(l, "ALL %u %u %u %u", &f, &wb, &bb, &why) == 4) {        // B applied a remote press to every candle
    for (auto &n : nodes) if (n.used) { n.flags = f; n.wbri = wb; n.bbri = bb; n.known = true; pubState(n); }
    gFlick = !(f & F_STEADY); pubFlicker();                            // remote Candle/Light -> HomeKit tile follows
    Serial.printf("ALL (remote) flags=%u wb=%u bb=%u\n", f, wb, bb);
    return;
  }
  if (sscanf(l, "ALLOFF %u", &why) == 1) {                             // B's group timer fired
    for (auto &n : nodes) if (n.used) { n.flags &= ~F_ON; pubState(n); }
    Serial.println("ALLOFF (timer)");
    return;
  }
  char fw[24] = "";                                                    // v5.2: optional 8th field from hub B v5.2
  int got = sscanf(l, "RPT %6s %u %u %u %u %d %u %23s", id, &f, &wb, &bb, &mv, &rssi, &why, fw);
  if (got < 7 || !validId(id)) return;
  if (got < 8 || !strcmp(fw, "-")) fw[0] = 0;
  Node *n = findNode(id, true); if (!n) return;
  n->mv = mv; n->rssi = rssi; n->seen = time(nullptr);
  if (fw[0] && strcmp(fw, n->fw)) {                                    // new / changed firmware -> device page too
    strlcpy(n->fw, fw, sizeof(n->fw)); n->announced = false;
    Serial.printf("candle %s firmware %s\n", id, fw);
  }
  if (!n->known || why == 4) { n->flags = f; n->wbri = wb; n->bbri = bb; n->known = true; }   // adopt; remote wins
  if (!n->announced) announce(*n);
  if (!n->online) pubAvail(*n, true);
  pubAttr(*n); pubState(*n);
  Serial.printf("RPT %s flags=%u wb=%u bb=%u mv=%u rssi=%d why=%u\n", id, f, wb, bb, mv, rssi, why);
}

void connectMqtt() {
  static uint32_t last = 0;
  if (!cfg.mhost[0] || mq.connected() || millis() - last < 5000) return;
  last = millis();
  Serial.print("MQTT connect... "); Serial.flush();
  if (mq.connect(cfg.name, cfg.muser[0] ? cfg.muser : nullptr, cfg.mpass[0] ? cfg.mpass : nullptr, T_STATUS, 1, true, "offline")) {
    Serial.println("ok");
    mq.publish(T_STATUS, "online", true);
    announceHub();
    for (auto &n : nodes) if (n.used && n.seen) {          // catch HA up on anything that changed offline
      announce(n); pubAvail(n, n.online); pubAttr(n); pubState(n);
    }
    char s[48];
    for (const char *leaf : {"+/set", "+/state", "+/base/set", "+/base/state"}) {
      snprintf(s, sizeof(s), "%s/%s", cfg.base, leaf); mq.subscribe(s);
    }
    gMqttUpAt = millis();                          // flicker/state seed has ~5 s to arrive, else publish defaults
    if (gSeeded) pubFlicker();
  } else Serial.printf("failed rc=%d\n", mq.state());
}

void setup() {
  Serial.begin(115200);
  LINK.setTxBufferSize(2048);                      // a 1 KB relay frame goes out without blocking the loop
  LINK.begin(115200, SERIAL_8N1, 16, 17);
  Serial.printf("fw %s\n", FW_TAG);
  pinMode(0, INPUT_PULLUP);                        // v5: BOOT button = setup (3 s) / factory reset (10 s)
  bool configured = cfgLoad();
  buildTopics();
  WiFi.mode(WIFI_STA); WiFi.setHostname(cfg.name); WiFi.setAutoReconnect(true);
  if (configured) WiFi.begin(cfg.ssid, cfg.pass);  // non-blocking: B link works before WiFi is up
  else portalStart("not configured");
  webSetup();                                      // v5: web server runs from boot (setup page needs it)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  mq.setServer(cfg.mhost, cfg.mport); mq.setBufferSize(1024); mq.setCallback(onMqtt);
  LINK.println("PING");
  Serial.printf("hub A v5.2 setup done (%s)\n", cfg.name);
}

// v5: BOOT (GPIO0) - held 3-10 s then released: open the setup network; held 10 s+: erase settings and reboot.
void buttonTick() {
  static uint32_t downAt = 0;
  bool pressed = digitalRead(0) == LOW;
  if (pressed && !downAt) downAt = millis() ? millis() : 1;
  if (!pressed && downAt) {
    uint32_t held = millis() - downAt; downAt = 0;
    if (held >= 10000) { cfgFactoryReset(); delay(200); ESP.restart(); }
    else if (held >= 3000) portalStart("BOOT button");
  }
}

// v5: open the setup network after 5 min without WiFi; close it once WiFi has been back for a while.
void portalTick(bool up) {
  static uint32_t downSince = 0, upSince = 0, lastKick = 0;
  uint32_t now = millis();
  if (up) { downSince = 0; if (!upSince) upSince = now; }
  else    { upSince = 0;   if (!downSince) downSince = now ? now : 1; }
  if (!up && cfg.ssid[0] && !portalOn && now - downSince > 300000) portalStart("WiFi down 5 min");
  if (!up && cfg.ssid[0] && portalOn && now - lastKick > 60000) { lastKick = now; WiFi.begin(cfg.ssid, cfg.pass); }
  if (portalOn) {
    dns.processNextRequest();
    if (up && now - upSince > 60000 && now - portalAt > 120000 && WiFi.softAPgetStationNum() == 0) portalStop();
  }
}

void loop() {
  static char buf[96]; static uint8_t len = 0; static uint32_t lastCheck = 0;
  static bool otaUp = false, wasUp = false;

  while (LINK.available()) {                       // B link first, WiFi or not
    char c = LINK.read();
    if (c == '\n' || c == '\r') { if (len) { buf[len] = 0; handleB(buf); len = 0; } }
    else if (len < sizeof(buf) - 1) buf[len++] = c;
  }

  otaTick();                                       // relay timeouts / retries (runs with or without WiFi)
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wasUp) { wasUp = up; Serial.printf("WiFi %s %s\n", up ? "up" : "down", up ? WiFi.localIP().toString().c_str() : ""); }
  buttonTick();
  portalTick(up);
  web.handleClient();                              // v5: always - the setup page must work without WiFi
  if (up) {
    if (!otaUp && cfg.otapass[0]) { ArduinoOTA.setHostname(cfg.name); ArduinoOTA.setPassword(cfg.otapass); ArduinoOTA.begin(); otaUp = true; }
    if (otaUp) ArduinoOTA.handle();
    connectMqtt();
    mq.loop();
    if (!gSeeded && mq.connected() && gMqttUpAt && millis() - gMqttUpAt > 5000) {   // nothing retained yet
      gSeeded = true; sendDepth(); pubFlicker(); Serial.println("flicker defaults published");
    }
  }

  static uint32_t lastHb = 0;
  if (millis() - lastHb > 10000) {
    lastHb = millis();
    Serial.printf("[hb] up=%lus wifi=%d mqtt=%d rc=%d lastB=%s\n", millis() / 1000, WiFi.status(), mq.connected(), mq.state(), lastB);
  }
  if (up && millis() - lastCheck > 60000) {        // mark silent candles offline
    lastCheck = millis(); time_t now = time(nullptr);
    for (auto &n : nodes)
      if (n.used && n.seen && n.seen < 1600000000 && now > 1600000000) n.seen = now;   // seen before NTP sync: restamp
      else if (n.used && n.online && n.seen && now - n.seen > (time_t)OFFLINE_AFTER_S) pubAvail(n, false);
  }
}
