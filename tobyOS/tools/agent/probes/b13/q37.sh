exec 2>&1 </dev/null
sleep 0 & p=$!; wait
r() { printf '%s\n' "$1" | sed "s/$p/PID/g"; }
r "1[${!}]"
r "2[${!-x}] [${!+y}] [${!:+z}] [${!:-w}]"
r "3[${#!}]"
a='!'; r "4[${!a}]"
a='$'; r "5[${!a}]" | sed "s/$$/SPID/"
a='-'; r "6[${!a}]" | tr -d hB
set -- one two
r "7[${!#}]"
x=7; r "8[${!?}]"
r "9[${!@}]"
r "10[${!*}]"
r "11[${!%x}]"
unset p
echo end
