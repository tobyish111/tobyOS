# An alias whose text has newlines is that many commands when eval runs
# it, as at the top level: `eval z` with z='unalias z<newline>n=3' unaliases
# z and sets n -- tsh ran `unalias z n=3`, one command (finding 467).
exec 2>&1
shopt -s expand_aliases
n=1
alias z='unalias -a
n=2'
eval z
echo "n=$n"
alias z='unalias z
n=3'
eval z
echo "n=$n"
alias m='echo a
echo b'
eval m
eval 'm; echo c'
eval m; echo d
alias fd='k=4
g() { echo "g $k"; }'
eval fd; g
