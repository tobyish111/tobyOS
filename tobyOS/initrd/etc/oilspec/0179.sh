declare -A A=(1 2 3)
echo status=$?
declare -p A

# bash-4.4 prints warnings to stderr but gives no indication of the problem
