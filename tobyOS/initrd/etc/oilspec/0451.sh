var='b'
b=global
echo $b
f() {
  local a=loc $var c=loc
  argv.py "$a" "$b" "$c"
}
f
