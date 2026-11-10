exec 2>&1 </dev/null
sleep 0 & p=$!
[[ ${!} == $p ]] && echo same1
[[ "${!}" == "$p" ]] && echo same2
echo "[${!:+set}] [${!:-def}] [${#!}]" | sed "s/$p/PID/g"
wait
x=y; y=z
echo "[${!x}]"
set -- a b
echo "[${!#}]"
echo "[${!@}]" "[${!*}]"
abc1=1 abc2=2
echo "[${!abc@}]" "[${!abc*}]"
echo "[${#}] [${?}] [${-}]" | tr -d 'hB'
echo "[${$}]" | sed "s/$$/PID/"
echo end
