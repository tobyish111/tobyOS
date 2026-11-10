# Note: Bash ignores other flags (-nrx) when variable names are supplied while
#   OSH uses other flags to select variables.  Bash's behavior is documented.
test_var1=111
readonly test_var2=222
export test_var3=333
declare -n test_var4=test_var1
f1() {
  local test_var5=555
  {
    echo '[declare -pn]'
    declare -pn test_var{0..5}
    echo '[declare -pr]'
    declare -pr test_var{0..5}
    echo '[declare -px]'
    declare -px test_var{0..5}
  } | grep -E '^\[|^\b.*test_var.\b'
}
f1
