var1() { echo func; }  # function names are NOT found.
declare -p var1 var2 >/dev/null
echo $?

var1=x
declare -p var1 var2 >/dev/null
echo $?

var2=y
declare -p var1 var2 >/dev/null
echo $?
