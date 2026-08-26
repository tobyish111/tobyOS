case $SH in mksh) exit ;; esac  # flakiness?

echo hi | { exit 99; } &
echo status=$?
wait %1
echo status=$?
