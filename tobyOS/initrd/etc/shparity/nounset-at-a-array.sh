# Under set -u, ${a@a} and ${a@A} of an array that has elements but no
# element 0 are the array's attributes and statement, not "a: unbound
# variable"; another operator, or an empty array, still is (finding 410).
exec 2>&1
set -u
declare -a foo=([1]=one); echo "[${foo@a}]" "[${foo@A}]"
declare -A m=([k]=v); echo "[${m@a}]"
bar=foo; echo "[${!bar@a}]"
( echo "[${foo@Q}]" ); echo "rc=$?"
( declare -a e=(); echo "[${e@a}]" ); echo "rc=$?"
