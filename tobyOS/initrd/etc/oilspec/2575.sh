shopt -s compat_array

declare -a array=(ale bean)

ref='array'  # when compat_array is on, this is like array[0]
ref_AT='array[@]'

echo ${!ref}
echo ${!ref_AT}
