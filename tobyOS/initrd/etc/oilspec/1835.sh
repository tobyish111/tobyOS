show_value() {
  local -n array_name=$1
  local idx=$2
  echo "${array_name[$idx]}"
}
shadock=(ga bu zo meu)
show_value shadock 2
