# Capture a candle's USB serial output to candle_log.txt (appends as it goes). Reopens the port if the candle reboots.
# Usage: .\candle_log.ps1 [-Port COM3] [-Seconds 240]      Settings: see candle.common.ps1 (ComPort)
param([int]$Seconds = 240, [string]$Port)
. "$PSScriptRoot\candle.common.ps1"
$Port = Get-CandleSetting -Name ComPort -Value $Port -EnvVar CANDLE_COM_PORT
$f = "$CandleFw\candle_log.txt"; "capture start $(Get-Date -f HH:mm:ss) on $Port" | Set-Content $f
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
  try {
    $c = New-Object System.IO.Ports.SerialPort $Port, 115200; $c.DtrEnable = $true; $c.Open()
    while ((Get-Date) -lt $end) { $s = $c.ReadExisting(); if ($s) { Add-Content $f $s -NoNewline }; Start-Sleep -Milliseconds 200 }
    $c.Close()
  } catch { Add-Content $f "`n[$(Get-Date -f HH:mm:ss) port gone: reconnecting]`n"; try { $c.Close() } catch {}; Start-Sleep 2 }
}
"`ncapture end $(Get-Date -f HH:mm:ss)" | Add-Content $f
