# Copy to candle.config.psd1 (git-ignored) and fill in. Any value can instead come from a -Parameter or env var.
# Do NOT put passwords here: use $env:CANDLE_OTA_PASS / $env:CANDLE_MQTT_PASS.
@{
  HubA          = '192.168.1.50'        # hub A's IP or hostname         (env: CANDLE_HUB_A)
  ComPort       = 'COM3'                # USB serial port for flashing/logging (env: CANDLE_COM_PORT)
  ArduinoCli    = 'arduino-cli'         # path to arduino-cli.exe, or name on PATH (env: CANDLE_ARDUINO_CLI)
  ArduinoConfig = ''                    # optional arduino-cli.yaml       (env: CANDLE_ARDUINO_CONFIG)
  Espota        = ''                    # espota.exe from the ESP32 core, for ota_hub_a.ps1 (env: CANDLE_ESPOTA)
                                        #   e.g. ...\Arduino15\packages\esp32\hardware\esp32\<ver>\tools\espota.exe
  MqttHost      = '192.168.1.10'        # broker host                     (env: CANDLE_MQTT_HOST)
  MqttPort      = 1883                  #                                  (env: CANDLE_MQTT_PORT)
  MqttUser      = ''                    #                                  (env: CANDLE_MQTT_USER)
}
