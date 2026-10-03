exec 2>&1
var=-f
[[ $var == -f ]] && echo t1
[[ '-f' == $var ]] && echo t2
[[ "-n" == -n ]] && echo t3
[[ $var ]] && echo t4
u=-z
eval '[[ $u x ]]'; echo "s5 $?"
eval 'op="=="; [[ a $op a ]]'; echo "s6 $?"
eval '[[ a "==" a ]]'; echo "s7 $?"
[[ -n == ]] && echo t8
[[ x\-f == x-f ]] && echo t9
eval '[[ \-f x ]]'; echo "s10 $?"
eval '[[ "-f" x ]]'; echo "s11 $?"
eval '[[ -f"" x ]]'; echo "s12 $?"
