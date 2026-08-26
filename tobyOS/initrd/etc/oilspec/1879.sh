case $SH in dash) exit ;; esac

show_hex() { od -A n -t c -t x1; }

echo -e '\0-' | show_hex
#echo -e '\x00-'
#echo -e '\000-'
