arr=(1 2 3)
eval -- "$(arr=(); arr[3]= arr[4]=foo; declare -p arr)"
for i in {0..4}; do
  echo "arr[$i]: ${arr[$i]+set ... [}${arr[$i]-unset}${arr[$i]+]}"
done
