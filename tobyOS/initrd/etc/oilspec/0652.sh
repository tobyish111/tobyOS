type mapfile >/dev/null 2>&1 || exit 0
printf '%s\r\n' {1..5..2} | {
  mapfile -t arr
  argv.py "${arr[@]}"
}
