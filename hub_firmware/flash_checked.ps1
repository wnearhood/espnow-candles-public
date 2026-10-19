# Flash a hub over USB only after the board on that port identifies itself (guards against flashing the wrong hub).
# Usage: .\flash_checked.ps1 -Sketch hub_b -Expect 'hub B' [-Port COM5]
#        .\flash_checked.ps1 -Sketch hub_a -Expect 'hub A' [-Port COM5]
# -Port omitted: uses the first serial port that is not ComPort (the candle's port).      Settings: see candle.common.ps1
param([ValidateSet('hub_a', 'hub_b')][string]$Sketch = 'hub_b', [string]$Expect = 'hub B', [string]$Port)
. "$PSScriptRoot\candle.common.ps1"
$log = "$CandleFw\flash_checked.log"
$skip = Get-CandleSetting -Name ComPort -EnvVar CANDLE_COM_PORT -Optional
"start $(Get-Date -f HH:mm:ss) want $Expect ports: $([System.IO.Ports.SerialPort]::GetPortNames())" | Set-Content $log
if (-not $Port) { $Port = [System.IO.Ports.SerialPort]::GetPortNames() | ? { $_ -ne $skip } | Select -First 1 }
if (-not $Port) { "no serial port found" | Add-Content $log; 'done' | Add-Content $log; exit 1 }
$oc = ''; try { $c = New-Object System.IO.Ports.SerialPort $Port, 115200; $c.Open(); $c.RtsEnable = $true; Start-Sleep -Milliseconds 100; $c.RtsEnable = $false; $end = (Get-Date).AddSeconds(8); while ((Get-Date) -lt $end) { $oc += $c.ReadExisting(); Start-Sleep -Milliseconds 100 }; $c.Close() } catch { "open fail $Port : $_" | Add-Content $log }
$id = $oc -split "`n" | ? { $_ -match 'hub A|hub B' } | Select -First 1
"$Port says: $id" | Add-Content $log
if ($id -notmatch $Expect) { "ABORT: $Port is not $Expect" | Add-Content $log; ($oc -split "`n" | Select -First 8) | Add-Content $log; 'done' | Add-Content $log; exit 1 }
& "$CandleFw\flash_v3.ps1" -Targets "${Sketch}:$Port"
Get-Content "$CandleFw\flash_v3.log" | Add-Content $log
'done' | Add-Content $log
