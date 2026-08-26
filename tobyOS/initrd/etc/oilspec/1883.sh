case $SH in dash) exit ;; esac
show_hex() { od -A n -t c -t x1; }

# OSH agrees with ZSH -- so you have the ability to print NUL bytes without
# legacy echo -e

echo $'\0' | show_hex
