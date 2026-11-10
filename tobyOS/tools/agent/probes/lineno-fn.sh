f() {
  echo "lineno $LINENO"
  nosuch_lf
}
f
printf "g() { echo \"g \$LINENO\"; }
" > /tmp/lsrc2.sh
. /tmp/lsrc2.sh
g
