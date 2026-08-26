myfunc() {
  local ref=$1
  echo ${!ref}
}
myfunc FUNCNAME
myfunc '?'
