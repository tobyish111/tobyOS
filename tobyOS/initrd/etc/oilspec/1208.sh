case $SH in mksh) exit ;; esac

set -o errtrace
trap 'echo line=$LINENO' ERR

( false; echo subshell )

x=$( false; echo command sub )

false
echo ok


# ash doesn't reject errtrace, but doesn't implement it
