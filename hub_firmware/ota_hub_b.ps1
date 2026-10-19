# Update hub B with no USB: compile hub_b, upload the .bin to hub A, A relays it to B over the A<->B UART.
# Usage: .\ota_hub_b.ps1 [-HubA <ip>] [-Pass <ota pass>] [-NoCompile]      Settings: see candle.common.ps1
param([string]$HubA, [string]$Pass, [switch]$NoCompile)
. "$PSScriptRoot\candle.common.ps1"
$HubA = Get-CandleSetting -Name HubA -Value $HubA -EnvVar CANDLE_HUB_A
$log = "$CandleFw\ota_hub_b.log"; $out = "$CandleFw\build\hub_b"
"start $(Get-Date -f HH:mm:ss) -> $HubA" | Set-Content $log
if (-not $NoCompile) {
  New-Item -ItemType Directory -Force $out | Out-Null
  (Invoke-ArduinoCli compile --fqbn $CandleFqbn.hub_b --output-dir $out "$CandleFw\hub_b" | Out-String) -split "`n" | ? { $_ -match 'error:|Sketch uses' } | Add-Content $log
}
$bin = "$out\hub_b.ino.bin"
if (-not (Test-Path $bin)) { "no $bin" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
$pass = Get-OtaPass $Pass
if (-not $pass) { "no OTA password: use -Pass or set CANDLE_OTA_PASS" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
"upload $((Get-Item $bin).Length) bytes" | Add-Content $log
(& curl.exe -s -m 120 -u "admin:$pass" -F "fw=@$bin" "http://$HubA/update-b") | Add-Content $log
$pass = $null
$t0 = Get-Date
do { Start-Sleep 3; $s = (& curl.exe -s -m 5 "http://$HubA/status") -join ''; "$(Get-Date -f HH:mm:ss) $s" | Add-Content $log }
while ($s -match 'begin|sending|verifying' -and ((Get-Date) - $t0).TotalSeconds -lt 300)
'done' | Add-Content $log
