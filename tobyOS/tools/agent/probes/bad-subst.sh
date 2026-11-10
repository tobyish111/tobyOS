echo ${%}; echo "a1 $?"
echo "a2 $?"
echo ${a&}; echo n
echo "a3 $?"
x=abc; echo ${#x-default}; echo n
echo "a4 $?"
echo ${#x:1:3}; echo n
echo "a5 $?"
echo ${x@Z}; echo n
echo "a6 $?"
echo ${x:}; echo n
echo "a7 $?"
a=(1 2); echo ${a[0][0]}; echo n
echo "a8 $?"
echo "${(m)x}"; echo n
echo "a9 $?"
y=${ echo hi; }; echo n
echo "a10 $?"
echo ${#a[0]/1/xxx}; echo n
echo "a11 $?"
echo end
