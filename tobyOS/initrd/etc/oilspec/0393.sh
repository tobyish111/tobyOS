# This illustrates an example usage of "eval & declare" for exporting
# multiple variables from $().
eval -- "$(
  printf '%s\n' a{1..10} | {
    sum=0 i=0 arr=()
    while read line; do
      ((sum+=${#line},i++))
      arr[$((i/3))]=$line
    done
    declare -p sum arr
  })"
echo sum=$sum
for ((i=0;i<${#arr[@]};i++)); do
  echo "arr[$i]=${arr[i]}"
done
