shopt -s expand_aliases  # bash requires this
x=x
alias echo-x='echo $x'  # nothing is evaluated here
x=y
echo-x hi
