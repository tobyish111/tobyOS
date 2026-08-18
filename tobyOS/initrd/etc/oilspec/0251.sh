declare -a a=(1 2 3 4)
eval 'declare -A a=([a]=x [b]=y [c]=z)'
echo status=$?
argv.py "${a[@]}"

declare -A A=([a]=x [b]=y [c]=z)
eval 'declare -a A=(1 2 3 4)'
echo status=$?
argv.py $(printf '%s\n' "${A[@]}" | sort)
