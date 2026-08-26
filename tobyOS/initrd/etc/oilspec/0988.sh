a=(a b c)
printf -v 'a[1]' %s 'foo'
echo status=$?
argv.py "${a[@]}"
