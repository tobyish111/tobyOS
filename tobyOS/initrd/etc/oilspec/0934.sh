case $SH in dash) exit ;; esac
sleep 0.1 &
builtin kill HUP $pid
echo $?
