# ro-decl.sh -- when does a declaration builtin's readonly error end the line?
readonly -a r=(1 2); readonly s=1
declare -a r=(7); echo "1 declare array, same line: $?"
echo "1b next line: $?"
declare s=2; echo "2 declare scalar, same line: $?"
export s=3; echo "3 export, same line: $?"
readonly s=4; echo "4 readonly, same line: $?"
typeset s=5; echo "5 typeset, same line: $?"
declare -a q=(1) r=(9) t=(2); echo "6 declare three, same line: $? [${q[*]}] [${t[*]-unset}]"
f() { declare -a r=(7); echo "7 declare in f: $?"; }; f; echo "7b after f: $?"
g() { local -a r=(9); echo "8 local in g: $?"; }; g; echo "8b after g: $?"
h() { local s=9; echo "9 local scalar in h: $?"; }; h; echo "9b after h: $?"
declare -a r=(7) || echo "10 || runs: $?"
echo done
