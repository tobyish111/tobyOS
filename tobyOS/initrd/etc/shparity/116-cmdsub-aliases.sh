alias a0='echo NOPE'
y=$(a0 2>/dev/null); echo "A0 [$y] $?"
shopt -s expand_aliases
alias echo_='echo [ '
cat <<EOF
$(echo_ ])
EOF
x=$(echo_ ])
echo "A2 [$x]"
echo "A3 $(echo_ a) b c"
alias q='printf %s\n'
echo "A4 [$(q "one  two")]"
f() { echo "A5 $(echo_ in fn)"; }; f
z=`echo_ tick`; echo "A6 $z"
