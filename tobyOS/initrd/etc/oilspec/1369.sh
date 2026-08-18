f() {
  # NOTE: local treated like a special builtin!
  E=env local v=var
  echo $E $v
}
f
