a=(x "" z); i=1; declare -A m=([k]=v [e]=)
echo "1 [${a[0]-d}] [${a[1]-d}] [${a[1]:-d}] [${a[5]-d}] [${a[5]:-d}]"
echo "2 [${a[0]+alt}] [${a[1]+alt}] [${a[1]:+alt}] [${a[5]+alt}]"
echo "3 [${m[k]-d}] [${m[e]:-d}] [${m[no]-d}] [${m[no]+alt}] [${m[k]:+alt}]"
echo "4 [${a[0]#x}] [${a[2]%z}] [${a[0]/x/X}] [${a[2]^^}] [${m[k]^}] [${a[0]:0:1}]"
echo "5 [${a[i]-d}] [${a[i]:-d}] [${a[i+1]-d}] [${a[-1]-d}]"
echo "6 [${a[7]=seven}] [${a[7]}] [${a[1]:=filled}] [${a[1]}] [${m[n]=new}] [${m[n]}]"
declare -p a
echo "7 [${a[0]@Q}] [${#a[2]}] [${m[k]@Q}]"
( echo "${a[9]?custom msg}" ) 2>/dev/null; echo "rc $?"
( echo "${m[zz]?}" ) 2>/dev/null; echo "rc $?"
( echo "${a[1]:?}" ) 2>/dev/null; echo "rc $?"
declare -n r=a; echo "8 [${r[0]-d}] [${r[9]-d}]"
b=(p q); echo "9 [${b[1]:-${a[0]:-none}}] [${b[5]:-${a[9]:-${m[k]}}}]"
f() { local -a l=(1); echo "10 [${l[0]-d}] [${l[3]-d}]"; }; f
unset 'a[0]'; echo "11 [${a[0]-gone}] [${a[0]:=back}] [${a[0]}]"
echo done
