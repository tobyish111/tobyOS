ref=1   # mksh makes this READ-ONLY!  Because it's not valid.

echo ref=$ref
typeset -n ref
echo ref=$ref

ref=foo
echo ref=$ref
