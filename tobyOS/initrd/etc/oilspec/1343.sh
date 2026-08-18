foo() {
  echo "function foo"
}
foo=bar
unset -v foo
echo foo=$foo
foo
