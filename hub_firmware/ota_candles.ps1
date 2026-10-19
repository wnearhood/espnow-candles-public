# Update ALL candles with no USB: compile candle, upload to hub A -> A relays to hub B (UART) -> B stores it and
# announces it -> each candle pulls it over ESP-NOW at its next check-in (<=10 s off, <=3 s lit), verifies, reboots.
# Usage: .\ota_candles.ps1 [-HubA <ip>] [-Pass <ota pass>] [-NoCompile]      Settings: see candle.common.ps1
# Remember: bump CANDLE_FW_VER in candle.ino first (the same build again is ignored by the candles).
param([string]$HubA, [string]$Pass, [switch]$NoCompile)
. "$PSScriptRoot\candle.common.ps1"
$HubA = Get-CandleSetting -Name HubA -Value $HubA -EnvVar CANDLE_HUB_A
$log = "$CandleFw\ota_candles.log"; $out = "$CandleFw\build\candle"
"start $(Get-Date -f HH:mm:ss) -> $HubA" | Set-Content $log
if (-not $NoCompile) {
  New-Item -ItemType Directory -Force $out | Out-Null
  (Invoke-ArduinoCli compile --fqbn $CandleFqbn.candle --output-dir $out "$CandleFw\candle" | Out-String) -split "`n" | ? { $_ -match 'error:|Sketch uses' } | Add-Content $log
}
$bin = "$out\candle.ino.bin"
if (-not (Test-Path $bin)) { "no $bin" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
$b = [IO.File]::ReadAllBytes($bin); "image fwId {0:X8}, {1} bytes, version line: {2}" -f [BitConverter]::ToUInt32($b,0xB0), $b.Length, ((Select-String -Path "$CandleFw\candle\candle.ino" -Pattern 'CANDLE_FW_VER "').Line.Trim()) | Add-Content $log
$pass = Get-OtaPass $Pass
if (-not $pass) { "no OTA password: use -Pass or set CANDLE_OTA_PASS" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
(& curl.exe -s -m 120 -u "admin:$pass" -F "fw=@$bin" "http://$HubA/update-candle") | Add-Content $log
$pass = $null
$t0 = Get-Date
do { Start-Sleep 3; $s = (& curl.exe -s -m 5 "http://$HubA/status") -join ''; "$(Get-Date -f HH:mm:ss) $s" | Add-Content $log }
while ($s -match 'begin|sending|verifying' -and ((Get-Date) - $t0).TotalSeconds -lt 300)
'done' | Add-Content $log
