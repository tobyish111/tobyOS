case $SH in dash|mksh) exit ;; esac

debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

echo a
echo b; echo c

echo d && echo e
echo f || echo g

(( h = 42 ))
[[ j == j ]]

var=value

readonly r=value
