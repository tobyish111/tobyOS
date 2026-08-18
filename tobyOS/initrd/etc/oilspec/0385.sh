# BUG? bash doesn't output anything for 'local/readonly -p var', which seems to
#   contradict with manual.  Besides, 'export -p var' is not described in
#   manual
test_var1=111
readonly test_var2=222
export test_var3=333
declare -n test_var4=test_var1
f1() {
  local test_var5=555
  {
    echo '[declare]'
    declare -p test_var{0..5}
    echo '[readonly]'
    readonly -p test_var{0..5}
    echo '[export]'
    export -p test_var{0..5}
    echo '[local]'
    local -p test_var{0..5}
  } | grep -E '^\[|^\b.*test_var.\b'
}
f1
