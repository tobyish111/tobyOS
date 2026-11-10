# A list written into a readonly variable of the function's own scope --
# `local -r x=(1); local x=(2) y=3`, or a readonly scalar local given a
# list -- is refused as bash expands the words, and like the same refusal
# at the top level it drops the function and the line that called it;
# under set -e the shell ends. A scalar value is the builtin's refusal and
# the function goes on. tsh refused each as the builtin, status 1, said
# local's refusal twice, and let the function run on (finding 625).
exec 2>&1
f() { local -r x=(1); local x=(2) y=3; echo "f went on: $?"; }
f; echo "the caller went on"
echo "after f: $?"; declare -p y
g() { declare -r x=1; declare x=(2) y=4; echo "g went on"; }
g; echo "the caller went on"
echo "after g: $?"; declare -p y
h() { typeset -r x=(1); typeset x=(5) y=5; echo "h went on"; }
h; echo "the caller went on"
echo "after h: $?"
k() { local -r x=1; local x=2 y=6; echo "k went on: $?"; declare -p y; }
k; echo "after k: $?"
( m() { local -r x=(1); local x=(2); echo "m went on"; }; m; echo "the subshell went on" ); echo "subshell: $?"
set -e
n() { local -r x=(1); local x=(3); echo "n went on"; }
n
echo "not reached"
