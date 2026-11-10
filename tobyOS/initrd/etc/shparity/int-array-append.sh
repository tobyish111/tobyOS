# An array literal assigned to an integer variable keeps the attribute: a
# later `x+=(3+4)` adds 7, as bash's does, where tsh's array lost it with
# the scalar (bash's appendop.tests; finding 289).
unset x; typeset -i x; x=([0]=7+11); x+=(3+4); echo "${x[@]}"
declare -p x
typeset -ia iarr=(2 2 3); iarr+=1; iarr+=(4 5 6); declare -p iarr
unset y; typeset -u y; y=(ab cd); y+=(ef); echo "${y[@]}"
