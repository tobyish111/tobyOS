exec 2>&1
dead() { sleep 1 & k=$!; kill -9 $k; e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
k=0
while (( k < 1 )); do
  sleep 1 & kill -9 $!; sleep 0.3
  k=$((k+1))
done
echo "after while"
if true; then
  sleep 1 & kill -9 $!; sleep 0.3
fi
echo "after if"
{
  sleep 1 & kill -9 $!; sleep 0.3
}
echo "after group"
case x in
  x) sleep 1 & kill -9 $!; sleep 0.3 ;;
esac
echo "after case"
for ((i=0; i<1; i++)); do
  sleep 1 & kill -9 $!; sleep 0.3
done
echo "after arith-for"
g()
{
  sleep 1 & kill -9 $!; sleep 0.3
}
g
echo "after g"
h() { g; sleep 1 & kill -9 $!; sleep 0.3; }
h
echo "after h"
(
  sleep 1 & kill -9 $!; sleep 0.3
)
echo "after subshell"
sleep 1 & kill -9 $!; sleep 0.3 | cat
echo "after pipe"
x=$(sleep 1 & kill -9 $!; sleep 0.3); echo "after comsub [$x]"
echo end
