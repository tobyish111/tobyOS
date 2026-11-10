export v1_global=0
f() {
  local v2_local=0
  export v2_local
  compgen -e v
}
f
