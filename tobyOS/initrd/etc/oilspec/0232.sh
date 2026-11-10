shopt -s strict_array

a=(1 10)
(( a++ ))  # doesn't make sense
echo "${a[@]}"
