# Compile candle, hub_b, hub_a; keep .bin files in build\<sketch>\ for wireless updates. Log: build_all.log
# Usage: .\build_all.ps1      Settings: see candle.common.ps1 (ArduinoCli, ArduinoConfig)
. "$PSScriptRoot\candle.common.ps1"
$log = "$CandleFw\build_all.log"
Copy-Item "$CandleFw\candle\candle_proto.h" "$CandleFw\hub_b\candle_proto.h" -Force   # hub B shares the candle protocol header
"start $(Get-Date -f HH:mm:ss)" | Set-Content $log
foreach ($s in 'candle', 'hub_b', 'hub_a') {
  "== $s" | Add-Content $log
  $out = "$CandleFw\build\$s"; New-Item -ItemType Directory -Force $out | Out-Null
  (Invoke-ArduinoCli compile --fqbn $CandleFqbn[$s] --output-dir $out "$CandleFw\$s" | Out-String) -split "`n" | ? { $_ -match 'error|Sketch uses' } | Select -First 12 | Add-Content $log
}
'done' | Add-Content $log
