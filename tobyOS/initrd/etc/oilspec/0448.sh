one=a
two='b c'
readonly $two $one
a=new
echo status=$?
b=new
echo status=$?
c=new
echo status=$?

# in OSH and zsh, this is an invalid variable name

# most shells make two variable read-only
