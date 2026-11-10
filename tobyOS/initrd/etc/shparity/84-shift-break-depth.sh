# POSIX shift and break/continue with counts at the edges: shift past $#
# fails and leaves the parameters alone; break/continue with n greater than
# the loop depth behave as if n were the depth; shift inside a function
# shifts the FUNCTION's own positionals.
set -- a b c
shift 2
echo "1=$?:$#:$1"
shift 5
echo "2=$?:$#:$1"
shift 0
echo "3=$?"
for i in 1 2; do
  for j in x y; do
    echo "4=$i$j"
    break 9
  done
done
echo "5=$?"
for i in 1 2; do
  for j in x y; do
    echo "6=$i$j"
    continue 9
  done
done
f() { shift; echo "7=$#:$1"; }
f p q r
echo "8=$#:$1"
echo end
