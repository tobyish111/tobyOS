foo() {
  echo bar
}

foo

declare -F
unset foo
declare -F

foo
