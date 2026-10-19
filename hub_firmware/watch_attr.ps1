# Log candles/<id>/attr every ~10 s (only when it changes) plus hub A status, to watch_attr.log. Read-only.
# Usage: .\watch_attr.ps1 -Id <candle id, e.g. 25ABCD> [-Minutes 10] [-HubA <ip>]      Settings: see candle.common.ps1
param([Parameter(Mandatory)][string]$Id, [int]$Minutes = 10, [string]$HubA)
. "$PSScriptRoot\candle.common.ps1"
$HubA = Get-CandleSetting -Name HubA -Value $HubA -EnvVar CANDLE_HUB_A
$log = "$CandleFw\watch_attr.log"; "start $(Get-Date -f HH:mm:ss)" | Set-Content $log
$end = (Get-Date).AddMinutes($Minutes); $last = ''
while ((Get-Date) -lt $end) {
  $a = (& "$CandleFw\mqtt_peek.ps1" -Filter "candles/$Id/attr" -Seconds 2 | Out-String).Trim()
  $st = (& curl.exe -s -m 4 "http://$HubA/status") -join ''
  $line = "$a | hub: $st"
  if ($line -ne $last) { "$(Get-Date -f HH:mm:ss) $line" | Add-Content $log; $last = $line }
  Start-Sleep 8
}
'done' | Add-Content $log
