declare d+=(d e)
echo "${d[@]}"
declare d+=(c l)
echo "${d[@]}"

readonly r+=(r e)
echo "${r[@]}"
# can't do this again

f() {
  local l+=(l o)
  echo "${l[@]}"

  local l+=(c a)
  echo "${l[@]}"
}

f
