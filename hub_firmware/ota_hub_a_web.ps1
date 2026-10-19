# Update hub A over HTTP (/update-a). Use this when espota (ota_hub_a.ps1) is blocked by a firewall.
# Usage: .\ota_hub_a_web.ps1 [-HubA <ip>] [-Pass <ota pass>] [-Bin <path to hub_a.ino.bin>]      Settings: see candle.common.ps1
param([string]$HubA, [string]$Pass, [string]$Bin)
. "$PSScriptRoot\candle.common.ps1"
$HubA = Get-CandleSetting -Name HubA -Value $HubA -EnvVar CANDLE_HUB_A
if (-not $Bin) { $Bin = "$CandleFw\build\hub_a\hub_a.ino.bin" }
$log = "$CandleFw\ota_hub_a_web.log"
"start $(Get-Date -f HH:mm:ss) -> $HubA $Bin" | Set-Content $log
if (-not (Test-Path $Bin)) { "no $Bin (run build_all.ps1 first)" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
$pass = Get-OtaPass $Pass
if (-not $pass) { "no OTA password: use -Pass or set CANDLE_OTA_PASS" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
"upload $((Get-Item $Bin).Length) bytes" | Add-Content $log
(& curl.exe -s -m 120 -u "admin:$pass" -F "fw=@$Bin" "http://$HubA/update-a") | Add-Content $log
$pass = $null
Start-Sleep 20
"$(Get-Date -f HH:mm:ss) status: $((& curl.exe -s -m 5 "http://$HubA/status") -join '')" | Add-Content $log
'done' | Add-Content $log
