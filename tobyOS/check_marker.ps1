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
    "programs/c_envtest/c_envtest.elf",
    "build/initrd/bin/c_envtest",
    "build/initrd.tar",
    "iso/boot/initrd.tar",
    "tobyOS.iso"
)
$needle = "M25C-debug-rev3"
foreach ($p in $paths) {
    $r = Find-String $p $needle
    "{0,-50}  {1}" -f $p, $r
}
