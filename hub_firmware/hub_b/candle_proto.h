// candle_proto.h - ESP-NOW message format shared by hub board B and every candle.
// v2 2026-10-11: separate wick + base LEDs, steady/flicker mode, IR timer request.
// Keep this file IDENTICAL in hub_b/ and candle/.
#pragma once
#include <stdint.h>

#define CP_CHANNEL       1      // fixed WiFi channel for ESP-NOW (hub B never joins WiFi)
#define CP_VERSION       4      // v3: group flicker depth in header. v4 2026-10-11: candle firmware over ESP-NOW
#define CP_MAGIC_STATE   0xC5   // hub -> all candles (broadcast, every 50 ms)
#define CP_MAGIC_REPORT  0xC6   // candle -> hub (unicast to the MAC the state came from)
#define CP_MAX_ENTRIES   40     // 10 + 40*6 = 250 bytes = the ESP-NOW payload limit
#define CP_DEPTH_DEFAULT 72     // flicker depth %: 72 -> wick dips to ~0.35 (the 2026-10-11 tuning)

// flags bits
#define CP_F_WICK   0x01        // wick (top) LED on
#define CP_F_BASE   0x02        // base (inner) LED on
#define CP_F_STEADY 0x04        // wick steady instead of flicker
#define CP_F_ON     (CP_F_WICK | CP_F_BASE)

// Candle ID = last 3 bytes of the candle's own WiFi MAC, shown in HA as 6 hex digits.
struct __attribute__((packed)) CpEntry {
  uint8_t id[3];
  uint8_t flags;   // CP_F_*
  uint8_t wbri;    // wick brightness 1-255
  uint8_t bbri;    // base brightness 1-255
};

struct __attribute__((packed)) CpState {
  uint8_t  magic;  // CP_MAGIC_STATE
  uint8_t  ver;    // CP_VERSION
  uint16_t seq;    // bumps on any change
  uint8_t  depth;  // group flicker depth 0-100 (% of full dip range)
  uint8_t  n;      // entries used
  uint32_t fwId;   // v4: candle firmware hub B holds (first 4 bytes of its ELF SHA-256, LE); 0 = none
  CpEntry  e[CP_MAX_ENTRIES];
};
#define CP_STATE_HDR    10
#define CP_STATE_LEN(n) (CP_STATE_HDR + (n) * sizeof(CpEntry))   // 10 + 40*6 = 250

// ---- v4 candle firmware update over ESP-NOW (candle pulls, hub B answers; B keeps no per-candle state) ----
// fwId = image bytes 0xB0..0xB3 = esp_app_desc_t.app_elf_sha256[0..3]. Candle updates when fwId != its own.
#define CP_MAGIC_OTAREQ  0xC8   // candle -> B: cnt 0 = send INFO, else send cnt DATA chunks from off
#define CP_MAGIC_OTAINFO 0xC9   // B -> candle
#define CP_MAGIC_OTADATA 0xCA   // B -> candle
#define CP_OTA_CHUNK     236    // 11-byte header + 236 = 247 <= 250
#define CP_OTA_WINDOW    8      // chunks per request

struct __attribute__((packed)) CpOtaReq  { uint8_t magic, ver; uint32_t fwId, off; uint8_t cnt; };
struct __attribute__((packed)) CpOtaInfo { uint8_t magic, ver; uint32_t fwId, size; uint8_t md5[16]; };
struct __attribute__((packed)) CpOtaData { uint8_t magic, ver; uint32_t fwId, off; uint8_t len; uint8_t d[CP_OTA_CHUNK]; };

// ---- v5 2026-10-12: long range (LR) + one-hop candle relay. CP_VERSION stays 4 on purpose: v5 only ADDS a
// magic and a reason bit, so v4 candles keep working next to v5 ones (they ignore what they don't know).
#define CP_MAGIC_RELAY   0xC7   // candle -> all (broadcast, LR): copy of a CpState the candle heard DIRECTLY from hub B
#define CP_R_RELAYED     0x80   // CpReport.reason bit: sent by a candle that can't hear hub B; relays forward it to B

// IR = state set by the remote on this candle; hub B copies it to EVERY candle.
enum CpReason : uint8_t { CP_R_APPLIED = 1, CP_R_DAILY = 2, CP_R_BOOT = 3, CP_R_IR = 4, CP_R_TIMER = 5,
                          CP_R_FWOK = 6 };   // v4: first boot of new firmware, verified (rollback cancelled)

struct __attribute__((packed)) CpReport {
  uint8_t  magic;  // CP_MAGIC_REPORT
  uint8_t  ver;
  uint8_t  id[3];
  uint8_t  flags;
  uint8_t  wbri;
  uint8_t  bbri;
  uint16_t mv;     // battery pack millivolts (0 = not measured)
  uint8_t  reason; // CpReason
  uint8_t  tmrH;   // IR timer button: off after this many hours (0 = none / cancel)
  // ---- 2026-10-13 (candle 4.11 / hub B v5.2): firmware identity. Older candles send only the first
  // CP_REPORT_V4_LEN bytes; receivers accept both lengths and zero-fill the tail (fwId 0 = unknown).
  uint32_t fwId;   // running image id (same scheme as CpState.fwId)
  char     fw[8];  // CANDLE_FW_VER, NUL-padded (not terminated if 8 chars)
};
#define CP_REPORT_V4_LEN 12
static inline bool cpReportLenOk(int len) { return len == CP_REPORT_V4_LEN || len == (int)sizeof(CpReport); }
