case $SH in dash|mksh|ash) exit ;; esac

trap 'false; echo $LINENO err' ERR
trap 'false; echo $LINENO debug' DEBUG

false
echo after=$?
