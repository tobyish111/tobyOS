unlocal() { unset "$@"; }

level2() {
  local hello=yy

  echo level2=$hello
  unlocal hello
  echo level2=$hello
}

level1() {
  local hello=xx

  level2

  echo level1=$hello
  unlocal hello
  echo level1=$hello

  level2
}

hello=global
level1

# bash, mksh, yash agree here.
