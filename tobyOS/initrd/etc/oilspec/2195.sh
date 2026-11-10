set -u

echo >tmp.sh '
g="global"
local L="local"

test_func() {
  echo "g = $g"
  echo "L = $L"
}
'

main() {
  # a becomes local here
  # test_func is defined globally
  . ./tmp.sh
}

main

# a is not defined
test_func
