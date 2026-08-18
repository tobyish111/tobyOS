a=(v{0,1,2,3,4,5,6,7,8,9})
unset -v 'a[3]' 'a[4]' 'a[7]' 'a[9]'

echo "${a[*]}"
IFS=
echo "${a[*]}"
IFS=/
echo "${a[*]}"
