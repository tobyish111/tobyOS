typeset -n ref

echo ref=$ref

# Now that it's a string, it still has the -n attribute
x=XX
ref=x
echo ref=$ref
