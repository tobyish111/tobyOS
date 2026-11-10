# $BASH_COMMAND is the command being run, as bash's printer writes it, for
# every kind of command (simple, [[ ]], (( )), for/case/select heads,
# pipeline stages, subshells) -- in DEBUG and ERR traps and in a function.
# tsh had no BASH_COMMAND at all.
# (batch 12; probe p76 in C:\t\b12)
exec 2>&1
trap 'echo "[D] $BASH_COMMAND"' DEBUG
x=a
case $x in a) echo ca;; esac
for ((i=0; i<1; i++)); do echo af; done
n=0; while (( n < 1 )); do n=1; done
until true; do :; done
( echo sub )
{ echo grp; }
f() { echo fdef; }
select s in one; do break; done <<< 1
[[ $x == a ]] && echo yes
! false
echo $( echo inner )
x=$(echo in2)
trap - DEBUG
