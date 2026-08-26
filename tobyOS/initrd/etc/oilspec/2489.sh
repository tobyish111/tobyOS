declare -A A=(['5']=3 ['6']=4)
mystr=abcdefg
echo assigned
echo ${mystr:$A:2}
