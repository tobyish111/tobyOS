$SH -c -z
case $? in
  1) echo flag-parsing-error ;;
  2) echo flag-parsing-error ;;
  *) echo fail ;;
esac

$SH -c 'echo 0=$0 1=$1' -z
echo status=$?

$SH -c 'echo 0=$0 1=$1' foo -z
echo status=$?
