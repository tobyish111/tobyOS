# A slice whose offset falls outside the value is nothing; past that, a
# negative length is "LEN: substring expression < 0", LEN as written, status
# 1 and the line dropped -- always for the positional parameters and an
# array, and for a value when its end comes before its offset. tsh took the
# offset as 0 and counted the length back (finding 404).
exec 2>&1
set -- a b c; echo ${@:1: -1 }; echo "not here"
echo "rc=$?"; echo "[${@:9:-1}]" "[${@: -9:-1}]" "[${@: -5}]"
echo "rc=$?"; echo ${*:1:-1}
echo "rc=$?"; a=(1 2 3 4); echo ${a[@]:1:-1}
echo "rc=$?"; n=-1; echo "${a[@]:0:n}"
echo "rc=$?"; s=([0]=1 [5]=2); echo "[${s[@]:6:-1}]" "[${s[@]: -7:-1}]"
echo "rc=$?"; echo "[${s[@]:5:-1}]"
echo "rc=$?"; declare -A m=([a]=1); echo "[${m[@]:2:-1}]"
echo "rc=$?"; echo "[${m[@]:1:-1}]"
echo "rc=$?"; x=abcdef; echo "[${x:2:-4}]" "[${x:9:-4}]" "[${x: -9:-4}]" "[${x: -9}]" "[${x: -6:2}]"
echo "rc=$?"; echo "[${x:4:-3}]"
echo "rc=$?"; n=-3; y=abc; echo "[${y:1:$n}]"
echo "rc=$?"; unset u; echo "[${u:0:-3}]"; u=; echo "[${u:0:-3}]"
echo "rc=$?"; b=(abcdef x); echo "[${b[0]:1:-3}]"; echo "[${b[0]:4:-3}]"
echo "rc=$?"
