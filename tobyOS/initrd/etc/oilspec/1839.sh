x=foo

ref=x

echo ref=$ref

typeset -n ref
echo ref=$ref

# mutate underlying var
x=bar
echo ref=$ref

typeset +n ref
echo ref=$ref
