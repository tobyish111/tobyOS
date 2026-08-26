case $SH in bash) exit ;; esac

shopt --set xtrace
echo SHELLOPTS=$SHELLOPTS
set -x
echo SHELLOPTS=$SHELLOPTS
set +x
echo SHELLOPTS=$SHELLOPTS
