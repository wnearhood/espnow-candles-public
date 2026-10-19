# candle.common.ps1 - shared settings for the hub_firmware scripts. Dot-source it:  . "$PSScriptRoot\candle.common.ps1"
# Each setting resolves in this order:  -Parameter  >  environment variable  >  candle.config.psd1  >  default  >  error.
# Copy candle.config.example.psd1 (repo root) to candle.config.psd1 and fill it in. candle.config.psd1 is git-ignored.
# Passwords are NEVER read from the config file: use -Pass / env vars, or the hub's secrets.h for a legacy build.
$CandleFw   = $PSScriptRoot                                   # ...\hub_firmware
$CandleRoot = Split-Path $PSScriptRoot -Parent                # repo root
$CandleCfg  = @{}
$cfgFile = Join-Path $CandleRoot 'candle.config.psd1'
if (Test-Path $cfgFile) { $CandleCfg = Import-PowerShellDataFile $cfgFile }

function Get-CandleSetting {
  param([string]$Name, [string]$Value, [string]$EnvVar, [string]$Default, [switch]$Optional)
  if ($Value) { return $Value }
  if ($EnvVar) { $e = [Environment]::GetEnvironmentVariable($EnvVar); if ($e) { return $e } }
  if ($CandleCfg.ContainsKey($Name) -and "$($CandleCfg[$Name])") { return "$($CandleCfg[$Name])" }
  if ($Default) { return $Default }
  if ($Optional) { return $null }
  throw "Setting '$Name' is not set. Pass -$Name, set `$env:$EnvVar, or add $Name to candle.config.psd1."
}

# Arduino CLI: ArduinoCli = exe path (default: arduino-cli on PATH); ArduinoConfig = optional arduino-cli.yaml.
function Invoke-ArduinoCli {
  param([Parameter(ValueFromRemainingArguments)][string[]]$CliArgs)
  $cli = Get-CandleSetting -Name ArduinoCli -EnvVar CANDLE_ARDUINO_CLI -Default 'arduino-cli'
  $yml = Get-CandleSetting -Name ArduinoConfig -EnvVar CANDLE_ARDUINO_CONFIG -Optional
  if ($yml) { & $cli --config-file $yml @CliArgs 2>&1 } else { & $cli @CliArgs 2>&1 }
}

# Board types per sketch.
$CandleFqbn = @{ candle = 'esp32:esp32:esp32c3:CDCOnBoot=cdc'; hub_a = 'esp32:esp32:esp32'; hub_b = 'esp32:esp32:esp32' }

# OTA password: -Pass > $env:CANDLE_OTA_PASS > hub_a\secrets.h (legacy). Never stored in config.
function Get-OtaPass([string]$Pass) {
  if ($Pass) { return $Pass }
  if ($env:CANDLE_OTA_PASS) { return $env:CANDLE_OTA_PASS }
  $s = Join-Path $CandleFw 'hub_a\secrets.h'
  if (Test-Path $s) { return ([regex]::Match((Get-Content $s -Raw), '#define\s+OTA_PASS\s+"([^"]*)"')).Groups[1].Value }
  return $null
}

# MQTT login: host/port/user from params > env > config > hub_a\secrets.h; password from $env:CANDLE_MQTT_PASS or secrets.h.
function Get-MqttLogin {
  $sec = ''; $s = Join-Path $CandleFw 'hub_a\secrets.h'; if (Test-Path $s) { $sec = Get-Content $s -Raw }
  function S($k) { if ($sec -match "#define\s+$k\s+`"?([^`"\r\n]+)`"?") { $Matches[1].Trim() } }
  $h  = Get-CandleSetting -Name MqttHost -EnvVar CANDLE_MQTT_HOST -Default (S 'MQTT_HOST')
  $p  = [int](Get-CandleSetting -Name MqttPort -EnvVar CANDLE_MQTT_PORT -Default ((S 'MQTT_PORT'), '1883' | ? { $_ } | Select -First 1))
  $u  = Get-CandleSetting -Name MqttUser -EnvVar CANDLE_MQTT_USER -Default (S 'MQTT_USER') -Optional
  $pw = if ($env:CANDLE_MQTT_PASS) { $env:CANDLE_MQTT_PASS } else { S 'MQTT_PASS' }
  [pscustomobject]@{ Host = $h; Port = $p; User = $u; Pass = $pw }
}
