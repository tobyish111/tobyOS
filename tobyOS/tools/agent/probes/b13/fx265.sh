d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# ${!} IS $! (batch 13), as ${!:-x}, ${!+x}, ${!%x} and ${#!} are: only a
# `!` before a name, a digit or one of # ? @ * is indirection. tsh called
# them all "bad substitution". ${!@} names the parameters joined by a
# blank. (logs/b13 q36 q37)
true & p=$!; wait
[[ ${!} == "$p" ]] && echo same
[[ "${!:-x}" == "$p" && "${!%x}" == "$p" && ${#!} == ${#p} ]] && echo ops
echo "[${!+set}] [${!:+set}]"
a='!'; [[ ${!a} == "$p" ]] && echo via-name
a='$'; [[ ${!a} == $$ ]] && echo dollar
set -- one two
echo "[${!#}]"
(echo "[${!@}]") 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
echo end
