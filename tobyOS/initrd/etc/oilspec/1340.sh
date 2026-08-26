# OSH has a RARE behavior here (matching yash and mksh), but at least it's
# consistent.

x=global
f() {
  local x=foo
  echo x=$x
  unset x
  echo x=$x
}
f
