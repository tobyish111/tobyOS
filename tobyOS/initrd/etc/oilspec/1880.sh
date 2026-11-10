case $SH in dash|ash) return ;; esac

# Show both printable and hex
show_hex() { od -A n -t c -t x1; }

printf $'x\U0z' | show_hex
echo ---

printf $'x\U00z' | show_hex
echo ---

printf $'\U0z' | show_hex
