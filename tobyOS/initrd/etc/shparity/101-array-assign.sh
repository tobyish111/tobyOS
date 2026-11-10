# Element assignment, sparse indices, append, and unset.
a[0]=zero
a[2]=two
echo "n=${#a[@]} 0=${a[0]} 1=[${a[1]}] 2=${a[2]}"
echo "idx=${!a[@]}"
a+=(appended)
echo "after-append n=${#a[@]} last=${a[3]}"
b=(x y z)
b+=(w)
echo "b n=${#b[@]} all=${b[*]}"
b[1]+=SUFFIX
echo "b1=${b[1]}"
unset 'b[1]'
echo "after-unset n=${#b[@]} idx=${!b[@]} all=[${b[*]}]"
unset b
echo "gone n=${#b[@]} all=[${b[*]}]"
echo end
