shopt -s expand_aliases  # bash requires this
x=x
y=y
alias echo-x='echo $x' echo-y='echo $y'
echo status=$?
echo-x X
echo-y Y
unalias echo-x echo-y
echo status=$?
echo-x X || echo undefined
echo-y Y || echo undefined
