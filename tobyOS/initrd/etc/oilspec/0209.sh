show-values() {
  echo values: ${A[@]}
}

declare -A A=(['K']=val)
show-values

declare ref='A["K"]'
echo ref ${!ref}

key=K
declare ref='A[$key]'
echo ref ${!ref}
