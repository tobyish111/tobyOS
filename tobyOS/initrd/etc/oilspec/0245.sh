# When evaluating the index, the modification to the array by the previous item
# of the initializer list is visible to the current item.
a=([0]=1+2+3 [a[0]]=10 [a[6]]=hello)
printf 'keys: '; argv.py "${!a[@]}"
printf 'vals: '; argv.py "${a[@]}"
