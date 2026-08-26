empty=''
fun() { echo hi; } > $empty
fun
echo status=$?
