case $SH in dash|mksh) exit ;; esac

set -x

dir=/
if [[ -d $dir ]]; then
  (( a = 42 ))
fi
