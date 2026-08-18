typeset -n ref
echo ref=$ref

# this is a no-op in bash, should be stricter
ref=x
echo ref=$ref

typeset -n ref2=undef
echo ref2=$ref2
ref2=x
echo ref2=$ref2


# mksh gives a good error: empty nameref target
