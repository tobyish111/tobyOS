n=2

show-strict() {
  shopt -p | grep 'strict_' | head -n $n
  echo -
}

show-strict
shopt -s strict:all
show-strict
shopt -u strict_argv
show-strict
