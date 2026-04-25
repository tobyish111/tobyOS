# pcap inspector for TCP segments. Like pcap_dns_dump.ps1 but for
# TCP. Decodes seq, ack, flags, window, payload length.
param([string]$Path = "net.pcap")

if (-not (Test-Path $Path)) { Write-Error "no $Path"; exit 1 }
$b = [System.IO.File]::ReadAllBytes($Path)
if ($b.Length -lt 24) { Write-Error "truncated"; exit 1 }

$linkType = [BitConverter]::ToUInt32($b, 20)
Write-Host "pcap linktype=$linkType bytes=$($b.Length)"

function FlagStr([byte]$f) {
    $s = ""
    if ($f -band 0x02) { $s += "S" }   # SYN
    if ($f -band 0x10) { $s += "A" }   # ACK
    if ($f -band 0x08) { $s += "P" }   # PSH
    if ($f -band 0x01) { $s += "F" }   # FIN
    if ($f -band 0x04) { $s += "R" }   # RST
    if ($f -band 0x20) { $s += "U" }   # URG
    if ($s -eq "") { $s = "." }
    return $s
}

$off = 24
$pkt = 0
while ($off + 16 -le $b.Length) {
    $ts_sec  = [BitConverter]::ToUInt32($b, $off)
    $ts_usec = [BitConverter]::ToUInt32($b, $off + 4)
    $incl    = [BitConverter]::ToUInt32($b, $off + 8)
    $off += 16
    if ($off + $incl -gt $b.Length) { break }
    $p = $b[$off..($off + $incl - 1)]
    $off += $incl
    $pkt++

    if ($p.Length -lt 14) { continue }
    $ethType = ([int]$p[12] -shl 8) -bor $p[13]
    if ($ethType -ne 0x0800) { continue }
    if ($p.Length -lt 14 + 20) { continue }
    $ihl = ($p[14] -band 0x0F) * 4
    $proto = $p[14 + 9]
    if ($proto -ne 6) { continue }                # TCP only
    if ($p.Length -lt 14 + $ihl + 20) { continue }
    $srcIp = "{0}.{1}.{2}.{3}" -f $p[14+12], $p[14+13], $p[14+14], $p[14+15]
    $dstIp = "{0}.{1}.{2}.{3}" -f $p[14+16], $p[14+17], $p[14+18], $p[14+19]
    $t = 14 + $ihl
    $sport = ([int]$p[$t] -shl 8) -bor $p[$t+1]
    $dport = ([int]$p[$t+2] -shl 8) -bor $p[$t+3]
    $seq = ([uint32]$p[$t+4] -shl 24) -bor ([uint32]$p[$t+5] -shl 16) `
         -bor ([uint32]$p[$t+6] -shl 8) -bor [uint32]$p[$t+7]
    $ackn = ([uint32]$p[$t+8] -shl 24) -bor ([uint32]$p[$t+9] -shl 16) `
         -bor ([uint32]$p[$t+10] -shl 8) -bor [uint32]$p[$t+11]
    $thlen = ($p[$t+12] -shr 4) * 4
    $flags = $p[$t+13]
    $win = ([int]$p[$t+14] -shl 8) -bor $p[$t+15]
    $tcpTotal = ([int]$p[14+2] -shl 8) -bor $p[14+3]
    $payloadLen = $tcpTotal - $ihl - $thlen
    Write-Host ("[{0,3}] t={1}.{2:000000} {3}:{4} -> {5}:{6} {7,-5} seq={8,10} ack={9,10} win={10,5} len={11,4}" -f `
        $pkt, $ts_sec, $ts_usec, $srcIp, $sport, $dstIp, $dport,
        (FlagStr $flags), $seq, $ackn, $win, $payloadLen)
}
