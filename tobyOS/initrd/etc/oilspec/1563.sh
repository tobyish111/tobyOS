foo() {
  set -e
  false
  echo "should be executed"
}
#foo && true
#foo || true

if foo; then
  true
fi

echo "should be executed"
