# Note: Bash-4.4+
type mapfile >/dev/null 2>&1 || exit 0
printf '%s\0' {1..5..2} | {
  mapfile -d '' arr
  echo "n=${#arr[@]}"
  printf '[%s]\n' "${arr[@]}"
}
