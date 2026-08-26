a=(0 1 2 3 4)
unset a[1]
unset a[4]
echo "${a[@]}"
echo -1 ${a[-1]}
echo -2 ${a[-2]}
echo -3 ${a[-3]}
echo -4 ${a[-4]}
echo -5 ${a[-5]}

a[-1]+=0  # append 0 on the end
echo ${a[@]}
(( a[-1] += 42 ))
echo ${a[@]}
