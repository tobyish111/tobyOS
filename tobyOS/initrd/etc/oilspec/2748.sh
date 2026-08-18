case $SH in dash) echo 'weird bug'; exit ;; esac

set -x
echo 1
unset PS4
echo 2
