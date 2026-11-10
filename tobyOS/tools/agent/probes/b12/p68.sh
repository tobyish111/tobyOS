exec 2>&1
f() {
  local -n q='#'; echo "1 s=$?"; declare -p q 2>&1
  local -n x=x; echo "2 s=$?"; declare -p x 2>&1
  local -n u=; echo "3 s=$?"; declare -p u 2>&1
  local -n e='a[1]'; echo "4 s=$?"; declare -p e 2>&1
  local -n b='1a'; echo "5 s=$?"
  local r=1; local -n r; echo "6 s=$?"; declare -p r 2>&1
  local h=@; local -n h; echo "7 s=$?"; declare -p h 2>&1
  local -n ok=target; echo "8 s=$?"; declare -p ok
}
f
