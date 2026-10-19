# Update hub A over Wi-Fi with ArduinoOTA (espota). Uses build\hub_a\hub_a.ino.bin. Log: ota_hub_a.log
# If your firewall blocks espota, use ota_hub_a_web.ps1 instead.
# Usage: .\ota_hub_a.ps1 [-HubA <ip>] [-Pass <ota pass>] [-Espota <path to espota.exe>]      Settings: see candle.common.ps1
param([string]$HubA, [string]$Pass, [string]$Espota)
. "$PSScriptRoot\candle.common.ps1"
$HubA   = Get-CandleSetting -Name HubA   -Value $HubA   -EnvVar CANDLE_HUB_A
$Espota = Get-CandleSetting -Name Espota -Value $Espota -EnvVar CANDLE_ESPOTA   # ...\packages\esp32\hardware\esp32\<ver>\tools\espota.exe
$log = "$CandleFw\ota_hub_a.log"
"start $(Get-Date -f HH:mm:ss) -> $HubA" | Set-Content $log
$pass = Get-OtaPass $Pass
if (-not $pass) { "no OTA password: use -Pass or set CANDLE_OTA_PASS" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
(& $Espota -i $HubA -p 3232 -a $pass -f "$CandleFw\build\hub_a\hub_a.ino.bin" -r 2>&1 | Out-String) -split "`n" | ? { $_ -match 'Error|Success|Authenticat|Upload|Waiting|done|fail' } | Select -Last 8 | Add-Content $log
$pass = $null
'done' | Add-Content $log
