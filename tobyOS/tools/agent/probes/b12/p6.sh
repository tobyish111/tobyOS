cd /tmp; mkdir -p /tmp/ds/a /tmp/ds/b /tmp/ds/c
HOME=/tmp/ds
pushd /tmp/ds/a; pushd /tmp/ds/b; pushd /tmp/ds/c
echo "~+=" ~+ "~-=" ~- "~1=" ~1 "~+2=" ~+2 "~-1=" ~-1
echo "DS=${DIRSTACK[*]} n=${#DIRSTACK[@]}"
dirs; dirs -l; dirs -v; dirs -p; dirs +1; dirs -0; dirs +3; dirs -lv; echo st=$?; dirs +9; echo st=$?; dirs x; echo st=$?
pushd +1; pushd -0; pushd +9; echo st=$?; pushd -n /tmp/ds; dirs
popd +1; popd -1; popd; popd -n; dirs; popd; popd; popd; echo st=$?
pushd; echo st=$?; pushd /nonexist; echo st=$?; pushd a b; echo st=$?
DIRSTACK[0]=/x; echo "${DIRSTACK[*]}"; unset DIRSTACK; dirs
