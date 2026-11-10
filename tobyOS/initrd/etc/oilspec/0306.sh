a=(0 1)
unset 'a[-1]'  # remove last element
a+=(2 3)
echo ${a[0]} $((a[0]))
echo ${a[1]} $((a[1]))
echo ${a[2]} $((a[2]))
echo ${a[3]} $((a[3]))
