i=1
a=(w x y z)
unset 'a[ i - 1 ]' a[i+1]  # note: can't have space between a and [
echo status=$?
echo "${a[@]}" len="${#a[@]}"
