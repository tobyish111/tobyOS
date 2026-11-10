case $SH in dash) exit ;; esac
show_hex() { od -A n -t c -t x1; }

nul=$'\0'
echo "$nul" | show_hex
printf '%s\n' "$nul" | show_hex
