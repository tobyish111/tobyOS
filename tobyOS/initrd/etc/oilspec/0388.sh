test_var1=111
readonly test_var2=222
export test_var3=333
declare -n test_var4=test_var1
f1() {
  local test_var5=555
  {
    echo '[declare -pn]'
    declare -pn
    echo '[declare -pr]'
    declare -pr
    echo '[declare -px]'
    declare -px
  } | grep -E '^\[|^\b.*test_var.\b'
}
f1
