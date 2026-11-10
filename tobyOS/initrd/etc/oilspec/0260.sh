case $SH in mksh) exit ;; esac

sp1[9]=x
typeset -p sp1 | sed 's/"//g'

sp1[-1]=A
sp1[-4]=B
sp1[-8]=C
sp1[-10]=D
typeset -p sp1 | sed 's/"//g'
