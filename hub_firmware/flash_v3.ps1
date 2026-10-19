# Compile and flash over USB. -Targets 'sketch:port[,sketch:port...]', e.g. 'candle:COM3' or 'hub_a:COM5,hub_b:COM6'.
# Candle: remove JP1 / batteries before plugging in USB. Log: flash_v3.log      Settings: see candle.common.ps1
param([Parameter(Mandatory)][string]$Targets)
. "$PSScriptRoot\candle.common.ps1"
$log = "$CandleFw\flash_v3.log"
"start $(Get-Date -f HH:mm:ss) $Targets" | Set-Content $log
foreach ($t in $Targets -split ',') {
  $sk, $port = $t -split ':'
  if (-not $CandleFqbn.ContainsKey($sk) -or -not $port) { "skip '$t' (expected sketch:port, sketch = candle|hub_a|hub_b)" | Add-Content $log; continue }
  "== $sk -> $port" | Add-Content $log
  (Invoke-ArduinoCli compile --upload -p $port --fqbn $CandleFqbn[$sk] "$CandleFw\$sk" | Out-String) -split "`n" | ? { $_ -match 'error:|Sketch uses|MAC:|Hash of data|Failed' } | Add-Content $log
}
"done $(Get-Date -f HH:mm:ss)" | Add-Content $log
