# A FAILED ASSIGNMENT STATEMENT discards the rest of the line, status 1
# (bash: expand_word_list_internal -> exp_jump_to_top_level (DISCARD)),
# whatever made it fail; set -e does not end the script for it.
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
declare e[1/0]=z; echo n6
echo "s6 $?"
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
