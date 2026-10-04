exec 2>&1
f() { echo "in f"; }
trap 'echo R' RETURN; f; trap - RETURN
echo ==; g() { trap 'echo R2' RETURN; echo in-g; }; g; f; trap -p RETURN; trap - RETURN
echo ==; set -T; trap 'echo R3' RETURN; f; trap - RETURN; set +T
echo ==; h() { trap -p DEBUG; }; trap 'true' DEBUG; h; trap - DEBUG
echo ==; k() { trap 'echo D2' DEBUG; }; k; trap -p DEBUG; trap - DEBUG
echo ==; declare -ft f 2>&1; trap 'echo D3 $BASH_COMMAND' DEBUG; f; trap - DEBUG; declare -F f; declare -pf f 2>&1 | head -1; declare +ft f
echo ==; set -k; echo a=1 b; set +k
echo ==; set -k; f2() { echo "v=${v-unset}"; }; f2 v=3; set +k
echo ==; set -k; export ka=1; echo "ka=${ka-unset}"; set +k
echo ==; bash -c 'set -t
echo one
echo two'; echo rc=$?
echo ==; printf 'set -t\necho one\necho two\n' > /tmp/q26t.sh; bash /tmp/q26t.sh; echo rc=$?
echo ==; printf 'set -H\necho hi\necho !!\necho "!!"\n' > /tmp/q26h.sh; bash /tmp/q26h.sh 2>&1; echo rc=$?
echo ==; printf 'set -o history -H\necho hi\necho !!\n' > /tmp/q26h2.sh; bash /tmp/q26h2.sh 2>&1; echo rc=$?
echo end
