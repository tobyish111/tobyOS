g() { echo "$# [$*]"; }
f() { g "$@"; }
f ""; f "" ""; f; f a ""; f "" a
set -- ""; g "$@"; g "$@"""; g "x$@"; g $@; g "${@}"
set --; g "$@"; g "$@"""; g "$@""$@"; g $@
one=(""); g "${one[@]}"; c=("${one[@]}"); echo "c ${#c[@]}"; e=(); g "${e[@]}"
two=("" ""); g "${two[@]}"; c=("${two[@]}"); echo "c ${#c[@]}"
g "${one[@]:0}"; g "${one[@]#x}"; x=(x); g "${x[@]#x}"; g "${@:1}"
for w in "${one[@]}"; do echo "w [$w]"; done
for w in "$@"; do echo "never"; done
set -- ""; for w in "$@"; do echo "p [$w]"; done
case "${one[@]}" in "") echo "case empty";; esac
v="${one[@]}"; echo "v [$v]"
e=(); a=(1 2)
c=("$@"""); echo "l1 ${#c[@]}"; c=("${e[@]}""${e[@]}"); echo "l2 ${#c[@]}"
c=("${e[@]}${a[@]}"); declare -p c; c=("${a[@]}${e[@]}"); declare -p c
c=(""); echo "l3 ${#c[@]}"; c=("" ""); echo "l4 ${#c[@]}"; c=(''"${e[@]}"); echo "l5 ${#c[@]}"
set -- ""; echo "l6 [${@:-minus}] [${*:-star}]"; set -- "" ""; echo "l7 [${@:-minus}]"
set -- a "" b; g "$@"; g "$*"; IFS=; g "$*"; unset IFS
f2() { local -a l=("$@"); echo "l8 ${#l[@]}"; }; f2 ""; f2; f2 "" ""
echo done
