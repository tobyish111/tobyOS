# https://stackoverflow.com/questions/32722007/is-skipping-ignoring-nul-bytes-on-process-substitution-standardized
# bash contains a warning!

show_bytes() {
  echo -n "$1" | od -A n -t x1
}

s=$(printf '.\001.')
echo len=${#s}
show_bytes "$s"

s=$(printf '.\000.')
echo len=${#s}
show_bytes "$s"

s=$(printf '\000')
echo len=${#s} 
show_bytes "$s"
