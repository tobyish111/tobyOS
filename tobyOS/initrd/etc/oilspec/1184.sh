case $SH in dash|mksh) exit ;; esac

debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

echo "result = $(echo command sub; echo two)"
( echo subshell
  echo two
)
echo done
