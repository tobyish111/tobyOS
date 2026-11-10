type mapfile >/dev/null 2>&1 || exit 0
printf '%s\n' a{0..10} | {
  mapfile -s 5 -n 3 -t arr
  echo "n=${#arr[@]}"
  printf '[%s]\n' "${arr[@]}"
}
