# Reading an element through a bad subscript: what is printed, what the
# expansion yields, whether the line goes on, and the status.
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
echo "r13 [${v[@]:-9}]"
echo "r14 $(( v[-9] + 1 ))"; echo "s14 $?"
echo end
