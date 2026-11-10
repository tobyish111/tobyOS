case $SH in dash|mksh) exit ;; esac

a=(1)
set -x
declare a+=(2)
