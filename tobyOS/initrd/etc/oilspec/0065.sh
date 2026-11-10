f() {
  local s+=foo
  echo s=$s

  set -u
  local s+=foo
  echo s=$s
}

f
