: prefix; myfunc() { echo serialized; }

code=$(typeset -f myfunc)

$SH -c "$code; myfunc"
