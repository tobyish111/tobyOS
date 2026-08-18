a=(1 2 3)
a[1]=5
a[40]=30  # out of order
a[10]=20
echo "${a[@]}" "${#a[@]}"  # length is 1
