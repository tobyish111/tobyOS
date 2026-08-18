f() {
  unset foo
}
foo=bar
echo foo=$foo
f
echo foo=$foo
