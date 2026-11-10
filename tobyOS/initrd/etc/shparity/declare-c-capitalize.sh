# declare -c capitalizes a value: its first character upper, the rest
# lower; -l or -u after it replace it, and +c takes away only that --
# tsh called -c an invalid option (finding 508).
exec 2>&1
T="Be Conservative in what you send and Liberal in what you accept"
declare -c q
q=$T; echo "$q"
declare -p q
q+=" MORE"; echo "$q"
declare -l q; q=$T; echo "$q"
declare -c q; declare -u q; q=$T; echo "$q"
declare +c q; q=$T; echo "$q"
f() { local -c l=aBC; echo "$l"; declare -p l; }; f
echo "${q@a}"; declare -c q; echo "${q@a}"
