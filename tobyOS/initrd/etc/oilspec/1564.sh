foo() {
  set -e
  false
  echo "should be executed"
}
! foo

echo "should be executed"
