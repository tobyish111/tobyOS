f() {
  # NOTE: local treated like a special builtin!
  local E=env > _tmp/r.txt
}
rm -f _tmp/r.txt
f
test -f _tmp/r.txt && echo REDIRECTED
