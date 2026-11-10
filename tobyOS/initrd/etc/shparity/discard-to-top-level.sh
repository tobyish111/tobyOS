# An arithmetic error in an array subscript, or in an -i variable's value,
# is bash's jump to the top level, not an ordinary expansion error: eval,
# `.`, a trap's action, `command eval` and a function do not stop it, so the
# whole line the script's reader was running goes and the next line sees
# status 1; a -c string ends with 1; a subshell it ends exits 1; no ERR
# trap runs; and set -e does not turn it into an exit. A $(( )) error's
# discard is still the one eval and `.` catch. tsh caught both kinds at
# eval and `.`, ran the ERR trap, exited under set -e and left a subshell's
# status 0 (finding 619).
exec 2>&1
eval 'a[1+]=x'; echo "eval went on"
echo "after eval: $?"
eval 'x=$((1+))'; echo "a \$(( )) error's eval goes on: $?"
echo 'a=( [1+]=x ); echo "file went on"' > top.src
. ./top.src; echo "the dot went on"
echo "after the dot: $?"
f() { eval 'echo ${a[2+]}'; echo "the function went on"; }
f; echo "the caller went on"
echo "after the function: $?"
command eval '(( a[3+] = 1 ))'; echo "command eval went on"
echo "after command eval: $?"
unset 'a[4+]'; echo "unset went on"
echo "after unset: $?"
declare -i n; eval 'n=1+'; echo "the -i value went on"
echo "after the -i value: $?"
declare -i m=2+; echo "declare -i went on"
echo "after declare -i: $?"
trap 'a[5+]=x; echo "the trap went on"' USR1
kill -USR1 $$; echo "the line the trap ran in went on"
echo "after the trap: $?"
trap - USR1
( eval 'a[6+]=x'; echo "the subshell went on" ); echo "subshell: $?"
"$BASH" -c 'eval "a[7+]=x"; echo "the -c string went on"
echo "its next line"' 2>&1 | sed 's/^[^:]*: //'; echo "-c: ${PIPESTATUS[0]}"
trap 'echo "ERR trap ran"' ERR
eval 'a[8+]=x'
echo "after the ERR trap's line: $?"
trap - ERR
set -e
eval 'a[9+]=x'
echo "set -e went on: $?"
