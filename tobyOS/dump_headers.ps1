$bytes = [System.IO.File]::ReadAllBytes("iso/boot/initrd.tar")
$names = @("bin/c_hello","bin/c_args","bin/c_filedemo","bin/c_alloctest","bin/c_envtest")
foreach ($target in $names) {
    $tBytes = [System.Text.Encoding]::ASCII.GetBytes($target)
    $found_off = -1
    for ($i = 0; $i -lt $bytes.Length - 200; $i += 512) {
        $match = $true
        for ($j = 0; $j -lt $tBytes.Length; $j++) {
            if ($bytes[$i + $j] -ne $tBytes[$j]) { $match = $false; break }
        }
        if ($match -and $bytes[$i + $tBytes.Length] -eq 0) { $found_off = $i; break }
    }
    if ($found_off -ge 0) {
        $size_field = ""
        for ($k = 124; $k -lt 136; $k++) { $size_field += [char]$bytes[$found_off + $k] }
        $name_field_end = ""
        for ($k = 95; $k -lt 100; $k++) { $name_field_end += ('{0:x2}' -f $bytes[$found_off + $k]) + ' ' }
        $size_hex = ""
        for ($k = 124; $k -lt 136; $k++) { $size_hex += ('{0:x2}' -f $bytes[$found_off + $k]) + ' ' }
        $line = "{0,-30} hdr_offset={1,8}  size_field='{2}'  hex='{3}'  name_end_hex='{4}'" -f $target, $found_off, $size_field, $size_hex.Trim(), $name_field_end.Trim()
        Write-Host $line
    }
    else {
        Write-Host ("{0,-30} NOT FOUND" -f $target)
    }
}
