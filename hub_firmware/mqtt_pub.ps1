# mqtt_pub.ps1 - publish ONE message (QoS 0, not retained) to the broker. 2026-10-17
# Broker/login: see Get-MqttLogin in candle.common.ps1 (password never printed).
# Usage: .\mqtt_pub.ps1 -Topic 'candles/<id>/base/set' -Payload '{"state":"ON","brightness":255}'
param([Parameter(Mandatory)][string]$Topic, [Parameter(Mandatory)][string]$Payload)
. "$PSScriptRoot\candle.common.ps1"
$m = Get-MqttLogin; $h = $m.Host; $p = $m.Port; $u = "$($m.User)"; $pw = "$($m.Pass)"; $m = $null
function Str([string]$s) { $b = [Text.Encoding]::UTF8.GetBytes($s); ,([byte[]](@([byte]($b.Length -shr 8), [byte]($b.Length -band 255)) + $b)) }
function RemLen([int]$n) { $o = @(); do { $d = $n % 128; $n = [math]::Floor($n / 128); if ($n -gt 0) { $d = $d -bor 128 }; $o += [byte]$d } while ($n -gt 0); ,([byte[]]$o) }
function Pkt([byte]$t, [byte[]]$body) { ,([byte[]](@($t) + (RemLen $body.Length) + $body)) }
$c = New-Object Net.Sockets.TcpClient($h, $p); $s = $c.GetStream(); $s.ReadTimeout = 2000
$vh = [byte[]](@(0, 4) + [Text.Encoding]::ASCII.GetBytes('MQTT') + @(4, 0xC2, 0, 60))
$body = [byte[]]($vh + (Str ("pub-" + (Get-Random))) + (Str $u) + (Str $pw)); $pw = $null
$k = Pkt 0x10 $body; $s.Write($k, 0, $k.Length)
$ack = New-Object byte[] 4; $n = $s.Read($ack, 0, 4)
if ($n -lt 4 -or $ack[3] -ne 0) { "CONNACK refused"; $c.Close(); exit 1 }
$k = Pkt 0x30 ([byte[]]((Str $Topic) + [Text.Encoding]::UTF8.GetBytes($Payload))); $s.Write($k, 0, $k.Length)
$k = [byte[]](0xE0, 0); $s.Write($k, 0, 2); Start-Sleep -Milliseconds 200; $c.Close()
"published $Topic"
