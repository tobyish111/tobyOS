f() {
  ( exit 42 )
  return
}
f
echo status=$?
