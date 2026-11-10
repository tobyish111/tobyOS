# A quoted list -- `declare -a x="(2)"` -- is the builtin's to make, so its
# refusal over a readonly x is the builtin's: "declare: x: readonly
# variable", status 1, and the line goes on, at the top level as in a
# function. readonly's and export's quoted list over a function's local is
# the GLOBAL's: the attribute goes on the local -- a readonly one too, with
# no refusal -- and the list into the global, with the attribute. And export
# marks a name it cannot assign exported all the same. tsh said no
# builtin's name, dropped the top-level line, refused readonly's and
# export's, and left the name unexported (finding 630).
exec 2>&1
declare -r x=(1); declare -a x="(2)" y=3; echo "same line: $?"
declare -p y
f() { local -r v=(1); local -a v="(2)"; echo "local: $?"; typeset -a v="(3)"; echo "typeset: $?"; }
f
g() { local -r w=(1); readonly -a w="(4)"; echo "readonly: $?"; declare -p w; }
g; declare -p w
h() { local -a u=(1); export -a u="(5)"; echo "export: $?"; declare -p u; }
h; declare -p u
k() { local -A t=([a]=1); readonly -A t="([k]=v)"; declare -p t; }
k; declare -p t
readonly s=1; export s=2; echo "export over readonly: $?"; declare -p s
m() { local -r q=1; export q=3; declare -p q; }
m
