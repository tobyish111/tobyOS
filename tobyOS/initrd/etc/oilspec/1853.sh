x=X
typeset -n -r ref=x

echo ref=$ref

# it feels like I shouldn't be able to mutate this?
ref=XX
echo ref=$ref

# now the underling variable is immutable
typeset -r x

ref=XXX
echo ref=$ref
echo x=$x
