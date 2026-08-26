typeset -r s1='12'
typeset -r s2='34'

s1='c'
echo status=$?
s2='d'
echo status=$?

s1+='e'
echo status=$?
s2+='f'
echo status=$?

unset s1
echo status=$?
unset s2
echo status=$?
