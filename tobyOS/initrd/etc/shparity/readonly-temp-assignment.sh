# A command's temporary assignment to a readonly name: bash looks the name
# up before it expands the value, so nothing in the value runs -- no $( ),
# no ${q:=v} -- and "NAME: readonly variable" comes where the assignment
# is, between the expansions before and after it; the command runs without
# it. A statement's value is expanded first, and then bash stops: the
# assignments after it are not expanded. Under posix mode a temporary one
# ends the shell, status 1. tsh expanded every value and then refused
# (finding 647).
exec 2>&1
readonly ro=1
ro=$(echo A >&2) echo "builtin ran"
ro=$(echo A >&2) /bin/echo "external ran"
ro=$(echo A >&2) :; echo "special builtin: $?"
f() { echo "function ran, ro=$ro"; }; ro=$(echo A >&2) f
x=$(echo X >&2) ro=$(echo A >&2) y=$(echo Y >&2) :; echo "in order: $?"
ro+=$(echo A >&2) echo "append ran"
ro=${q:=set} echo "q=$q"
declare -ra ra=(1); ra=$(echo A >&2) echo "array ran"
g() { local -r lr=1; lr=$(echo A >&2) echo "local ran"; }; g
ro=$(echo statement >&2) x2=$(echo X2 >&2); echo "statement: x2=$x2"
( ro=$(echo A >&2) exec /bin/echo "exec ran" )
( set -o posix; ro=$(echo A >&2) echo "posix: not reached" ); echo "posix: $?"
