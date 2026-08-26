declare -A a
a=([j]=1 2 3 4)
echo "status=$?"
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
# Bash outputs warning messages and succeeds (exit status 0)
