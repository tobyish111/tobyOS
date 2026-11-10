# $LINENO inside a function body is that line of the body, wherever the
# function is called from: tsh reported the line of the CALL for every
# command in the body (found while S12 put line numbers on diagnostics).
f() {
  echo "f body line $LINENO"
  echo "f second line $LINENO"
}
f
g() { echo "g one-liner $LINENO"; }
g
printf 'h() {\n  echo "h in a sourced file $LINENO"\n}\n' > /tmp/fx174.sh
. /tmp/fx174.sh
h
k() (
  echo "subshell body $LINENO"
)
k
m() {
  for i in 1; do
    echo "loop in a function $LINENO"
  done
  echo "after the loop $LINENO"
}
m
r() { echo "redirected body $LINENO"; } 2>/dev/null
r
n() {
  m
  echo "after a nested call $LINENO"
}
n
echo "after the calls $LINENO"
rm -f /tmp/fx174.sh
echo done
