# An arithmetic error in ${P:OFF:LEN} names the parameter as it was written
# -- "x: 1+: syntax error...", "a[@]:", "@:", "!r:" -- and an array's slice
# drops the line with status 1, where tsh carried on with 0. An expansion
# inside that fails is one error, not two (finding 403).
exec 2>&1
x=abc; echo "${x:1+}"
echo "rc=$?"; set -- p q; echo "${@:1+}"
echo "rc=$?"; echo "${*:1:2+}"
echo "rc=$?"; a=(1 2); echo "${a[@]:1+}"
echo "rc=$?"; echo "${a[*]:0:*2}"
echo "rc=$?"; declare -A m=([k]=v); echo "${m[@]:1+}"
echo "rc=$?"; r=x; echo "${!r:1+}"
echo "rc=$?"; echo "${1:1+}"
echo "rc=$?"; b=(abc def); echo "${b[1]:1+}"
echo "rc=$?"; e=; echo "${e:1+}"
echo "rc=$?"; echo "[${x:$((1+))}]"
echo "rc=$?"
