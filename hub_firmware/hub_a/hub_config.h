// hub_config.h - v5 2026-10-12: hub A settings live in NVS (the ESP32's settings flash), not in the build.
// One firmware image fits every hub; each hub is configured from its setup page (see hub_a.ino "setup page").
//
// Keys (namespace "hubcfg"): ssid pass mhost mport muser mpass otapass name base appass noseed
//   name   = hostname, MQTT client id, ArduinoOTA name    (must be unique per hub)
//   base   = MQTT topic prefix, e.g. "candles"             (must be unique per hub)
//   appass = password of the hub's own setup network (random on first boot)
//
// Migration: if NVS is empty and secrets.h is present at build time, its values seed NVS ONCE.
// After a factory reset ("noseed" set) secrets.h is never used again. A build WITHOUT secrets.h is generic.
#pragma once
#include <Preferences.h>
#include <esp_random.h>
#include <esp_mac.h>
#if __has_include("secrets.h")
#include "secrets.h"
#define HUB_HAVE_SECRETS 1
#endif

struct HubCfg {
  char ssid[33], pass[65];
  char mhost[64]; uint16_t mport; char muser[33], mpass[65];
  char otapass[33];
  char name[32];
  char base[25];
  char appass[17];
};
HubCfg cfg;
static Preferences prefs;

static void cfgGet(const char *k, char *dst, size_t n, const char *def) {
  String v = prefs.getString(k, def); strlcpy(dst, v.c_str(), n);
}

static void cfgSave() {
  prefs.putString("ssid", cfg.ssid);   prefs.putString("pass", cfg.pass);
  prefs.putString("mhost", cfg.mhost); prefs.putUShort("mport", cfg.mport);
  prefs.putString("muser", cfg.muser); prefs.putString("mpass", cfg.mpass);
  prefs.putString("otapass", cfg.otapass);
  prefs.putString("name", cfg.name);   prefs.putString("base", cfg.base);
  prefs.putString("appass", cfg.appass);
}

// Last two MAC bytes, e.g. "3fa2" - used for fresh-hub defaults and the setup network name.
static void macTail(char *out, bool upper) {
  uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_STA);
  sprintf(out, upper ? "%02X%02X" : "%02x%02x", m[4], m[5]);
}

static void newApPass(char *out, size_t n) {          // 10 chars, no look-alikes (0/O, 1/l/I)
  static const char A[] = "abcdefghjkmnpqrstuvwxyz23456789";
  size_t len = min((size_t)10, n - 1);
  for (size_t i = 0; i < len; i++) out[i] = A[esp_random() % (sizeof(A) - 1)];
  out[len] = 0;
}

// Returns true when WiFi is configured.
bool cfgLoad() {
  prefs.begin("hubcfg", false);
  char tail[5]; macTail(tail, false);
#ifdef HUB_HAVE_SECRETS
  if (!prefs.isKey("ssid") && !prefs.getBool("noseed", false)) {   // existing hub: one-time move into NVS
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid)); strlcpy(cfg.pass, WIFI_PASS, sizeof(cfg.pass));
    strlcpy(cfg.mhost, MQTT_HOST, sizeof(cfg.mhost)); cfg.mport = MQTT_PORT;
    strlcpy(cfg.muser, MQTT_USER, sizeof(cfg.muser)); strlcpy(cfg.mpass, MQTT_PASS, sizeof(cfg.mpass));
    strlcpy(cfg.otapass, OTA_PASS, sizeof(cfg.otapass));
    strlcpy(cfg.name, "candle-hub-a", sizeof(cfg.name));   // keeps today's hostname, client id, topics, HA entities
    strlcpy(cfg.base, "candles", sizeof(cfg.base));
    newApPass(cfg.appass, sizeof(cfg.appass));
    cfgSave();
    Serial.println("cfg: settings moved from secrets.h into NVS (one time)");
  }
#endif
  char defName[32], defBase[25];
  snprintf(defName, sizeof(defName), "hub-%s", tail); snprintf(defBase, sizeof(defBase), "hub_%s", tail);
  cfgGet("ssid", cfg.ssid, sizeof(cfg.ssid), "");       cfgGet("pass", cfg.pass, sizeof(cfg.pass), "");
  cfgGet("mhost", cfg.mhost, sizeof(cfg.mhost), "");    cfg.mport = prefs.getUShort("mport", 1883);
  cfgGet("muser", cfg.muser, sizeof(cfg.muser), "");    cfgGet("mpass", cfg.mpass, sizeof(cfg.mpass), "");
  cfgGet("otapass", cfg.otapass, sizeof(cfg.otapass), "");
  cfgGet("name", cfg.name, sizeof(cfg.name), defName);  cfgGet("base", cfg.base, sizeof(cfg.base), defBase);
  if (!prefs.isKey("appass")) { newApPass(cfg.appass, sizeof(cfg.appass)); prefs.putString("appass", cfg.appass); }
  cfgGet("appass", cfg.appass, sizeof(cfg.appass), "");
  Serial.printf("cfg: name=%s base=%s ssid=%s mqtt=%s:%u user=%s ota=%s\n", cfg.name, cfg.base,
                cfg.ssid[0] ? cfg.ssid : "(none)", cfg.mhost[0] ? cfg.mhost : "(none)", cfg.mport,
                cfg.muser[0] ? cfg.muser : "-", cfg.otapass[0] ? "set" : "NOT SET");
  return cfg.ssid[0] != 0;
}

void cfgFactoryReset() {                                // BOOT held 10 s
  prefs.clear();
  prefs.putBool("noseed", true);                        // never re-seed from secrets.h after an explicit reset
  Serial.println("cfg: factory reset - all settings erased");
}

// HA ids for the hub itself. base "candles" keeps the v1-v4 ids so existing HA entities / HomeKit tiles survive.
static bool cfgLegacy() { return !strcmp(cfg.base, "candles"); }
void hubUid(char *out, size_t n) { if (cfgLegacy()) strlcpy(out, "candle_hub", n); else snprintf(out, n, "%s_hub", cfg.base); }
const char *hubDisplayName() { return cfgLegacy() ? "Candle hub" : cfg.name; }
