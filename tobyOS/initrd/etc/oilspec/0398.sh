var1() { echo func; }  # function names are NOT found.
typeset -p var1 var2 >/dev/null
echo $?

var1=x
typeset -p var1 var2 >/dev/null
echo $?

var2=y
typeset -p var1 var2 >/dev/null
echo $?
