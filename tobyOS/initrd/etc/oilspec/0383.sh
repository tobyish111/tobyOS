# BUG: bash doesn't output flags with "local -p", which seems to contradict
#   with manual.
test_var1=111
readonly test_var2=222
export test_var3=333
declare -n test_var4=test_var1
f1() {
  local test_var5=555
  {
    echo '[declare]'
    declare -p
    echo '[readonly]'
    readonly -p
    echo '[export]'
    export -p
    echo '[local]'
    local -p
  } | grep -E '^\[|^\b.*test_var.\b'
}
f1
