case $SH in dash) exit ;; esac

$SH >stdout 2>stderr <<'EOF'

s=$'a\x03b\004c\x00d'
set -o xtrace
echo "$s"
EOF

show_hex() { od -A n -t c -t x1; }

echo STDOUT
cat stdout | show_hex
echo

echo STDERR
grep 'echo' stderr 
