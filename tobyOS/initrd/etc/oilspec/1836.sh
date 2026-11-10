set1() {
  local -n array_name=$1
  local val=$2
  array_name[1]=$val
}
shadock=(a b c d)
set1 shadock ZZZ
echo ${shadock[@]}
