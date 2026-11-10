unset undef
echo status=$?

a=(x y z)
unset 'a[99]'  # out of range
echo status=$?

unset 'not_array[99]'  # not an array
echo status=$?
