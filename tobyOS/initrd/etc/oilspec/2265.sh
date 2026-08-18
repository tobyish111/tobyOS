prog='
case $- in
  *e*) echo e ;;
esac

case $- in
  *f*) echo f ;;
esac
'

# normal way
$SH -o errexit -o noglob -c "$prog"

# odd way
$SH -oo errexit noglob -c "$prog"



e
