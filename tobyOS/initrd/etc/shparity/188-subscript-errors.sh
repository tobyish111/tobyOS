# ARRAY SUBSCRIPTS THAT GO WRONG, by bash's arrayfunc.c: a subscript that
# does not evaluate DISCARDS the command (status 1, set -e or not); one
# still negative after counting back is "bad array subscript" (report_error:
# set -e ends the shell) -- for a read the expansion is empty and the line
# goes on, for an assignment the statement fails and its line is dropped;
# an element of a SCALAR makes it an array whose element 0 is the old
# value; and an element cannot go into a command's environment (`c[1]=y
# cmd` is not a valid identifier). tsh fell through to the scalar path:
# status 0, the line ran on, and most of these said nothing.
exec 2>&1
s=str; s[0]=q; echo "1 [$s] [${s[0]}] [${#s[@]}]"
t=str; t[1]=q; echo "2 [$t] [${t[0]}] [${t[1]}] [${#t[@]}]"
u=str; u+=(x); echo "3 [$u] [${u[1]}]"
w=str; w[-1]=z; echo "4 [$w] [${w[*]}]"
declare -p s t u w
c[1]=y true; echo "p1 $? [${c[*]}]"
c[1/0]=y true; echo "p2 $?"
echo "p3 $?"
a[]=x; echo n4
echo "p4 $?"
s=str; s[-1]=q; echo "p5 $? [$s]"
t=str; t[-2]=q; echo n6
echo "p6 $? [$t]"
f() { u[1/0]=z; echo in-f; }; f; echo n7
echo "p7 $?"
v=(1 2 3)
echo "${v[1/0]}"; echo n8
echo "p8 $?"
echo "${v[-9]}" n9
echo "p9 $?"
w=(1); w[-1]+=x; echo "p10 $? ${w[*]}"
echo end
v=(1 2 3)
echo "r1 [${v[-9]}]"; echo "s1 $?"
echo "r2 [${v[-3]}] [${v[-1]}]"
echo "r3 [${#v[-9]}]"; echo n3
echo "s3 $?"
echo "r4 [${v[-9]:-d}]"; echo "s4 $?"
s=str
echo "r5 [${s[-1]}]"; echo "s5 $?"
echo "r6 [${s[0]}] [${#s[0]}] [${s[1]}]"
unset u
echo "r7 [${u[-1]}]"; echo "s7 $?"
echo "r8 [${#u[-1]}]"; echo "s8 $?"
e=()
echo "r9 [${e[-1]}]"; echo "s9 $?"
echo "r10 [${v[1/0]:-d}]"; echo n10
echo "s10 $?"
echo "r11 [${#v[1/0]}]"; echo n11
echo "s11 $?"
i=-9
echo "r12 [${v[i]}]"; echo "s12 $?"
echo "r14 $(( v[-9] + 1 ))"; echo "s14 $?"
echo end
echo ---
a=(1 2); a=x; declare -p a; echo "[$a] [${#a[@]}]"
b=abc; b+=(d e f); declare -p b; echo "[$b]"
declare -A m2; m2[k]=v; m2=z; echo "[${m2[0]}] [${m2[k]}] [${#m2[@]}]"
aa=(p q); typeset -n rr=aa; rr=y; declare -p aa
c=(1 2); c=3; unset c; echo "[${c-gone}] [${c[1]-gone}]"
e=(9 8); for e in 5; do :; done; declare -p e
f=(1 2); f+=z; declare -p f
echo ---
a[1/0]=x; echo n1
echo "s1 $?"
b=(1); b[-5]=q; echo n2
echo "s2 $? [${b[*]}]"
x=1 c[2*0x]=y; echo n3
echo "s3 $?"
readonly r=1
r=2; echo n4
echo "s4 $? r=$r"
d[1+]=z; echo n5
echo "s5 $?"
f[1/0]=x g=2; echo n7
echo "s7 $? g=${g-unset}"
h=(1 2); h[-1]=last; echo "s8 $? ${h[*]}"
i=$(( 1/0 )); echo n9
echo "s9 $?"
set -e
j[1/0]=y; echo n10
echo "s10 $?"
k=(1); k[-3]=z; echo n11
echo "s11 $?"
set +e
echo end
