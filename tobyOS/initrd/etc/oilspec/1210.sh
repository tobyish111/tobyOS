case $SH in mksh) exit ;; esac

set -o errtrace
trap 'echo line=$LINENO' ERR

false & wait

{ false; echo async; } & wait
