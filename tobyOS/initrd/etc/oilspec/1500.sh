case $SH in dash|bash|mksh|ash) exit ;; esac

$SH -o errexit -O strict_errexit -c 'echo a; export x=$(might-fail); echo b'
echo status=$?
$SH -o errexit -O strict_errexit -c 'echo a; readonly x=$(might-fail); echo b'
echo status=$?
$SH -o errexit -O strict_errexit -c 'echo a; x=$(true); echo b'
echo status=$?
