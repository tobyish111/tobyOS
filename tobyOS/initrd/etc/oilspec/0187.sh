declare -A a

a=([aa]=b [foo]=bar ['a+1']=c)
echo a="${a}"

a=([0]=zzz)
echo a="${a}"

a=(['0']=yyy)
echo a="${a}"
