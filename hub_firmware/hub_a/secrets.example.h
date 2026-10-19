// secrets.example.h - OPTIONAL since hub A v5 (settings are set on the hub's setup page and kept in NVS).
// If secrets.h exists at build time, it seeds an EMPTY hub's settings once (migration). Never commit secrets.h.
#pragma once
#define WIFI_SSID   "your-iot-ssid"
#define WIFI_PASS   "your-wifi-password"
#define MQTT_HOST   "192.168.1.10"
#define MQTT_PORT   1883
#define MQTT_USER   "mqtt-user"
#define MQTT_PASS   "mqtt-password"
#define OTA_PASS    "ota-password"     // hub A web/ArduinoOTA uploads; the ota_*.ps1 scripts read it from secrets.h
