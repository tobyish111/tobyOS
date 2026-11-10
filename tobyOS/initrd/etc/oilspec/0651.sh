type mapfile >/dev/null 2>&1 || exit 0
printf '%s\n' {1..5..2} | {
  mapfile -t arr
  echo "n=${#arr[@]}"
  printf '[%s]\n' "${arr[@]}"
}
