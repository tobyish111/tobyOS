# Tiny pcap dumper -- prints DNS-port (53 or 53999) packets only.
# No external deps; just enough to validate the wire path is alive.
param([string]$Path = "net.pcap")

if (-not (Test-Path $Path)) { Write-Error "no $Path"; exit 1 }
$b = [System.IO.File]::ReadAllBytes($Path)
if ($b.Length -lt 24) { Write-Error "truncated"; exit 1 }

# Global header: 4 magic + 2+2 ver + 4 thiszone + 4 sigfigs + 4 snaplen + 4 linktype
$magic = [BitConverter]::ToUInt32($b, 0)
if ($magic -ne 0xA1B2C3D4) { Write-Warning ("unknown magic 0x{0:X8}" -f $magic) }
$linkType = [BitConverter]::ToUInt32($b, 20)
Write-Host "pcap: magic=0x$('{0:X8}' -f $magic) linktype=$linkType bytes=$($b.Length)"

$off = 24
$pkt = 0
while ($off + 16 -le $b.Length) {
    $ts_sec  = [BitConverter]::ToUInt32($b, $off)
    $ts_usec = [BitConverter]::ToUInt32($b, $off + 4)
    $incl    = [BitConverter]::ToUInt32($b, $off + 8)
    $orig    = [BitConverter]::ToUInt32($b, $off + 12)
    $off += 16
    if ($off + $incl -gt $b.Length) { Write-Warning "truncated tail"; break }
    $p = $b[$off..($off + $incl - 1)]
    $off += $incl
    $pkt++

    if ($p.Length -lt 14) { continue }
    $ethType = ([int]$p[12] -shl 8) -bor $p[13]
    if ($ethType -ne 0x0800) { continue }       # IPv4 only
    if ($p.Length -lt 14 + 20) { continue }
    $ihl = ($p[14] -band 0x0F) * 4
    $proto = $p[14 + 9]
    if ($proto -ne 17) { continue }             # UDP only
    if ($p.Length -lt 14 + $ihl + 8) { continue }
    $srcIp = "{0}.{1}.{2}.{3}" -f $p[14+12], $p[14+13], $p[14+14], $p[14+15]
    $dstIp = "{0}.{1}.{2}.{3}" -f $p[14+16], $p[14+17], $p[14+18], $p[14+19]
    $u = 14 + $ihl
    $sport = ([int]$p[$u] -shl 8) -bor $p[$u+1]
    $dport = ([int]$p[$u+2] -shl 8) -bor $p[$u+3]
    if ($sport -ne 53 -and $sport -ne 53999 -and $dport -ne 53 -and $dport -ne 53999) {
        continue
    }
    $d = $u + 8
    $dnsId = if ($p.Length -ge $d + 2) { ([int]$p[$d] -shl 8) -bor $p[$d+1] } else { 0 }
    $flags = if ($p.Length -ge $d + 4) { ([int]$p[$d+2] -shl 8) -bor $p[$d+3] } else { 0 }
    $qd    = if ($p.Length -ge $d + 6) { ([int]$p[$d+4] -shl 8) -bor $p[$d+5] } else { 0 }
    $an    = if ($p.Length -ge $d + 8) { ([int]$p[$d+6] -shl 8) -bor $p[$d+7] } else { 0 }
    $rcode = $flags -band 0x0F
    $qr    = ($flags -shr 15) -band 1
    Write-Host ("[{0,3}] t={1}.{2:000000} {3}:{4} -> {5}:{6} dns id=0x{7:X4} qr={8} rcode={9} qd={10} an={11} bytes={12}" -f `
        $pkt, $ts_sec, $ts_usec, $srcIp, $sport, $dstIp, $dport, $dnsId, $qr, $rcode, $qd, $an, $incl)
}
