# bash runs an external command in a child of its own and does the
# command's redirections there, so an error expanding one of their words --
# a here-string's, a file name's -- is the child's: the command fails with
# status 1 and the line goes on, judged by set -e and the ERR trap as any
# failure. A builtin's, a function's or a group's redirections are the
# shell's own, and their error discards the line; a subshell's are its
# child's. tsh discarded every one -- or ended the shell for `${x:?}` -- and
# gave a group and a subshell status 2 (finding 629).
exec 2>&1
cat <<< ${a[}; echo "cat's here-string: $?"
cat < ./in$((1+)); echo "cat's input: $?"
cat <<< ${zz:?gone}; echo "cat's \${x:?}: $?"
echo hi > ./out${a[}; echo "not after echo's"
echo "after echo's: $?"
f() { cat; }; f <<< ${a[}; echo "not after the function's"
echo "after the function's: $?"
{ cat; } <<< ${a[}; echo "not after the group's"
echo "after the group's: $?"
( cat ) <<< ${a[}; echo "the subshell's: $?"
trap 'echo "ERR trap: $?"' ERR
cat <<< $((2/0)); echo "with an ERR trap: $?"
trap - ERR
set -e
cat <<< ${a[}
echo "not reached"
