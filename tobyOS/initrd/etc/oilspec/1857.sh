typeset -n ref1=ref2
typeset -n ref2=ref1  # not detected here
echo defined $?
ref1=z  # detected here
echo mutated $?
