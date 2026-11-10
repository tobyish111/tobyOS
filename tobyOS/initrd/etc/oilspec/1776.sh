case $SH in bash) exit ;; esac

$SH --rcdir $TMP/__does-not-exist -i -c 'echo hi'
echo status=$?
