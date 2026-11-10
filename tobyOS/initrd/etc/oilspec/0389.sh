declare -a test_var6=()
declare -A test_var7=()
f1() {
  {
    echo '[declare -pa]'
    declare -pa
    echo '[declare -pA]'
    declare -pA
  } | grep -E '^\[|^\b.*test_var.\b'
}
f1
