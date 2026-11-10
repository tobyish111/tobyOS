# Subscripted assignments in other places: as a command's prefix (bash
# does not take an array element into a temporary environment), with an
# empty subscript, on a scalar, inside a function, and reads.
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
unset nosuch; nosuch[-1]=q; echo n11
echo "p11 $? [${nosuch[*]-unset}]"
echo end
