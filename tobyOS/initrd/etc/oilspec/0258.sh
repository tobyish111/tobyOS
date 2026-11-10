sp1[10]=a
sp1[20]=b
sp1[99]=c
typeset -p sp1 | sed 's/"//g'
sp1+=(1 2 3)
typeset -p sp1 | sed 's/"//g'
