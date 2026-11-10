# This is a regression for a crash
# But actually there is also an incompatibility -- we don't print anything

declare x
declare -p x

function f { local x; declare -p x; }
x=1
f
