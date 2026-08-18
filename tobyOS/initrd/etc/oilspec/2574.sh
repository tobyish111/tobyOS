shopt -s strict_array

declare -a array=(ale bean)
ref='array'
echo ${!ref}
