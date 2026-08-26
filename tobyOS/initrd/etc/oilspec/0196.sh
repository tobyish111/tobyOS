declare -a array=(1 2 3)
declare -A assoc
assoc[42]=43
assoc["${array[@]}"]=foo

echo "${assoc["${array[@]}"]}"
for entry in "${!assoc[@]}"; do
  echo $entry
done | sort
