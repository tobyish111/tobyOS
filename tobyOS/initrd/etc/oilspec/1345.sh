a=(x y z)
unset 'a[1]'
echo status=$?
echo "${a[@]}" len="${#a[@]}"
