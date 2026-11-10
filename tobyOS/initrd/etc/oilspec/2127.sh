# Fix for flaky tests: dash behaves non-deterministically under load!  It
# doesn't implement the behavior anyway so I don't care why.
case $SH in
  *dash)
    exit 1
    ;;
esac

echo "ok" > $TMP/f.txt
stdout_stderr.py &>> $TMP/f.txt
grep ok $TMP/f.txt >/dev/null && echo 'ok'
grep STDOUT $TMP/f.txt >/dev/null && echo 'ok'
grep STDERR $TMP/f.txt >/dev/null && echo 'ok'
