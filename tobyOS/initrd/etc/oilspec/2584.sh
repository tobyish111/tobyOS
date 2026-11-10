x=y
y=x
echo cycle=${!x}

typeset -n a=b
typeset -n b=a
echo cycle=${a}
