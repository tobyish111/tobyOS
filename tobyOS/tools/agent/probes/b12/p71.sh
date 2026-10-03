f1() { echo hi; }
f2() {
  local x=1   # comment
  if [ "$x" = 1 ]; then echo one; elif false; then :; else echo other; fi
  for i in a b; do echo $i; done
  while false; do break; done
  case $x in a|b) echo ab;; *) echo star;; esac
  { echo grp; } >/dev/null
  ( echo sub )
  echo a | cat
  true && false || echo x
  [[ -n $x && $x == 1 ]]
  (( x += 1 ))
  cat <<EOF
body $x
EOF
  for ((i=0;i<2;i++)); do :; done
  echo "q 'x'" 'y "z"' $'t\t' >&2 2>/dev/null
  f1 &
  return 0
}
f3() ( echo subshell-body )
function f4 { echo kw; }
f5() { :; } >/dev/null
declare -f f1 f2 f3 f4 f5
type f1
