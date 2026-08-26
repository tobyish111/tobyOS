# This is DIFFERENT than the nameref itself being 'array[0]' !

array=(X Y Z)
typeset -n ref=array
ref[0]=xx
echo ${array[@]}
