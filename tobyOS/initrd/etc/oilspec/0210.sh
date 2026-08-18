show-values() {
  echo values: ${assoc[@]}
}

declare -A assoc=(['K']=val)
show-values

printf -v 'assoc["K"]' '/%s/' val2
show-values

key=K
printf -v 'assoc[$key]' '/%s/' val3
show-values

# Somehow bash doesn't allow this
#prefix=as
#printf -v '${prefix}soc[$key]' '/%s/' val4
#show-values
