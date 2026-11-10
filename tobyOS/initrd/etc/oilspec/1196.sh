case $SH in dash|mksh|ash) exit ;; esac

trap 'false; echo $LINENO usr1' USR1
trap 'false; echo $LINENO err' ERR

sh -c "kill -USR1 $$"
echo after=$?
