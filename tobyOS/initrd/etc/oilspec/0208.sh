show-values() {
  echo values: ${A[@]}
}

declare -A A=(['K']=val)
show-values

declare -n ref='A["K"]'
echo before $ref
ref='val2'
echo after $ref
show-values

echo ---

key=K
declare -n ref='A[$key]'
echo before $ref
ref='val3'
echo after $ref
show-values
