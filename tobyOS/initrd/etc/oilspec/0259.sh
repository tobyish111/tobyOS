sp1[10]=a
sp1[20]=b
sp1[30]=c
typeset -p sp1 | sed 's/"//g'
sp1[10]=X
sp1[25]=Y
sp1[90]=Z
typeset -p sp1 | sed 's/"//g'
