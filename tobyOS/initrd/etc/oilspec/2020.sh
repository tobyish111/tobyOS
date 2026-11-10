$SH -c 'echo ${@@P}' dummy a b c
echo status=$?
$SH -c 'echo ${*@P}' dummy a b c
echo status=$?
$SH -c 'a=(x y); echo ${a@P}' dummy a b c
echo status=$?
