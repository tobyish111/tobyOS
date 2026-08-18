# 8 is not a valid octal digit

printf '%b\n' '\558'
printf '%b\n' '\0558'
echo

show_bytes() {
  od -A n -t x1
}
printf '%b' '\7' | show_bytes
printf '%b' '\07' | show_bytes
printf '%b' '\007' | show_bytes
printf '%b' '\0007' | show_bytes
