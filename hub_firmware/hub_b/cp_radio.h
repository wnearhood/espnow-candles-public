// cp_radio.h - v5 2026-10-12: ESP-NOW long range (LR) helpers. Keep this file IDENTICAL in hub_b/ and candle/.
// LR = Espressif-only 802.11 mode (1/2 and 1/4 Mbps; ESP-IDF Wi-Fi guide: ~4 dB better sensitivity than 11b,
// ~2-2.5x the 11b distance). Radios run 11b/g/n + LR together, which per the same guide stays compatible with
// normal-rate frames - so v4 firmware (normal rate only) and v5 firmware can share the air during the rollout.
// TX rate is set per peer with esp_now_set_peer_rate_config() (IDF 5.4+; this core 3.3.12 = IDF 5.5).
// Include AFTER candle_proto.h (needs CP_CHANNEL).
#pragma once
#include <esp_wifi.h>
#include <esp_now.h>

#define CP_LR_RATE WIFI_PHY_RATE_LORA_250K   // 1/4 Mbps: max range. A 12-candle state frame (82 B) is ~3 ms on air.

static volatile bool cpTxDone = false;
static void cpOnSent(const esp_now_send_info_t *, esp_now_send_status_t) { cpTxDone = true; }

// Call right after WiFi.mode(WIFI_STA), before setting the channel / esp_now_init().
static void cpEnableLR() {
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
}

static void cpAddPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {}; memcpy(p.peer_addr, mac, 6); p.channel = CP_CHANNEL; p.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&p);
}

// Send one frame at normal (11b 1 Mbps, the ESP-NOW default) or LR rate; wait up to 25 ms for it to leave.
// Needs esp_now_register_send_cb(cpOnSent). The peer (incl. broadcast) must already exist.
static bool cpSendAt(const uint8_t *mac, const void *d, size_t n, bool lr) {
  esp_now_rate_config_t c = {};
  c.phymode = lr ? WIFI_PHY_MODE_LR : WIFI_PHY_MODE_11B;
  c.rate    = lr ? CP_LR_RATE : WIFI_PHY_RATE_1M_L;
  esp_now_set_peer_rate_config(mac, &c);
  cpTxDone = false;
  if (esp_now_send(mac, (const uint8_t *)d, n) != ESP_OK) return false;
  uint32_t t0 = millis();
  while (!cpTxDone && millis() - t0 < 25) delay(1);
  return cpTxDone;
}

// Both rates: reaches v4 receivers (normal only) and far v5 receivers (LR).
static void cpSendBoth(const uint8_t *mac, const void *d, size_t n) { cpSendAt(mac, d, n, false); cpSendAt(mac, d, n, true); }
