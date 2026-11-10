shopt -s eval_unsafe_arith || true

show-len() {
  echo len=${#assoc[@]}
}

declare -A assoc=(['K']=val)
show-len

unset -v 'assoc["K"]'
show-len

declare -A assoc=(['K']=val)
show-len
key=K
unset -v 'assoc[$key]'
show-len

declare -A assoc=(['K']=val)
show-len
unset -v 'assoc[$(echo K)]'
show-len

# ${prefix} doesn't work here, even though it does in arithmetic
#declare -A assoc=(['K']=val)
#show-len
#prefix=as
#unset -v '${prefix}soc[$key]'
#show-len
