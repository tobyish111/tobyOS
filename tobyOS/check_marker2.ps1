function Find-String($path, $needle) {
    if (-not (Test-Path $path)) { return "MISSING" }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $needleBytes = [System.Text.Encoding]::ASCII.GetBytes($needle)
    for ($i = 0; $i -lt ($bytes.Length - $needleBytes.Length); $i++) {
        $hit = $true
        for ($j = 0; $j -lt $needleBytes.Length; $j++) {
            if ($bytes[$i+$j] -ne $needleBytes[$j]) { $hit = $false; break }
        }
        if ($hit) { return "found at $i" }
    }
    return "NOT FOUND"
}

$paths = @(
    "tobyOS.iso",
    "iso/boot/initrd.tar",
    "iso/boot/tobyos.bin",
    "tobyos.bin"
)
foreach ($p in $paths) {
    "{0,-50}  rev3-marker={1}    data@-marker={2}" -f $p, (Find-String $p "M25C-debug-rev3"), (Find-String $p "data@%p")
}
