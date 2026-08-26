show_hex() { od -A n -t c -t x1; }

printf '\0\n' | show_hex
