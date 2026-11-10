# cd with no operand: HOME unset is "cd: HOME not set", status 1, and an
# empty HOME leaves the directory as it is, status 0 -- tsh went to / for
# both (finding 417).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
( unset HOME; cd; echo "rc=$? $([ "$PWD" = "$d" ] && echo same)" )
( HOME=; cd; echo "rc=$? $([ "$PWD" = "$d" ] && echo same)" )
( HOME=/nonexistent-home; cd; echo "rc=$?" )
cd /; rm -rf "$d"
