set -C
echo foo >| $TMP/no-clobber
exec 3<> $TMP/no-clobber
read -n 1 <&3
echo -n . >&3
exec 3>&-
cat $TMP/no-clobber
