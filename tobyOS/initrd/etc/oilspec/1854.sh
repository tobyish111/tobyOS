x=X
typeset -n ref=x
echo ref=$ref

# this works
unset ref
echo ref=$ref
echo x=$x
