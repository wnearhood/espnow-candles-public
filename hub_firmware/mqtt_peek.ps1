# mqtt_peek.ps1 - read-only: dump retained candles/# topics from the broker. 2026-10-12
# Broker/login: see Get-MqttLogin in candle.common.ps1 (password never printed). Usage: .\mqtt_peek.ps1 [-Filter 'candles/#'] [-Seconds 3]
param([string]$Filter = 'candles/#', [int]$Seconds = 3)
. "$PSScriptRoot\candle.common.ps1"
$m = Get-MqttLogin; $h = $m.Host; $p = $m.Port; $u = "$($m.User)"; $pw = "$($m.Pass)"; $m = $null
function Str([string]$s) { $b = [Text.Encoding]::UTF8.GetBytes($s); ,([byte[]](@([byte]($b.Length -shr 8), [byte]($b.Length -band 255)) + $b)) }
function RemLen([int]$n) { $o = @(); do { $d = $n % 128; $n = [math]::Floor($n / 128); if ($n -gt 0) { $d = $d -bor 128 }; $o += [byte]$d } while ($n -gt 0); ,([byte[]]$o) }
function Pkt([byte]$t, [byte[]]$body) { ,([byte[]](@($t) + (RemLen $body.Length) + $body)) }
$c = New-Object Net.Sockets.TcpClient($h, $p); $s = $c.GetStream(); $s.ReadTimeout = 500
$vh = [byte[]](@(0, 4) + [Text.Encoding]::ASCII.GetBytes('MQTT') + @(4, 0xC2, 0, 60))
$body = [byte[]]($vh + (Str ("peek-" + (Get-Random))) + (Str $u) + (Str $pw)); $pw = $null
$k = Pkt 0x10 $body; $s.Write($k, 0, $k.Length)
$k = Pkt 0x82 ([byte[]](@(0, 1) + (Str $Filter) + @(0))); $s.Write($k, 0, $k.Length)
$buf = New-Object Collections.Generic.List[byte]; $tmp = New-Object byte[] 8192; $end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { try { $n = $s.Read($tmp, 0, $tmp.Length); if ($n -le 0) { break }; $buf.AddRange([byte[]]$tmp[0..($n - 1)]) } catch { } }
$c.Close(); $a = $buf.ToArray(); $i = 0; $out = @()
while ($i -lt $a.Length) {
  $t = $a[$i]; $i++; $mul = 1; $len = 0; do { $d = $a[$i]; $i++; $len += ($d -band 127) * $mul; $mul *= 128 } while ($d -band 128)
  $st = $i; $i += $len
  if (($t -shr 4) -eq 3) { $tl = $a[$st] * 256 + $a[$st + 1]; $topic = [Text.Encoding]::UTF8.GetString($a, $st + 2, $tl); $ps = $st + 2 + $tl; if (($t -band 6) -ne 0) { $ps += 2 }
    $out += "{0} = {1}" -f $topic, [Text.Encoding]::UTF8.GetString($a, $ps, $st + $len - $ps) }
  elseif (($t -shr 4) -eq 2 -and $a[$st + 1] -ne 0) { "CONNACK refused (code $($a[$st+1]))" }
}
$out | Sort-Object
