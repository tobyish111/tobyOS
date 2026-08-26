show_bytes() {
  echo -n "$1" | od -A n -t x1
}

s=$(printf '\000.\000')
echo len=${#s}
show_bytes "$s"

echo ---

t=${s#?}
echo len=${#t}
show_bytes "$t"

t=${s##?}
echo len=${#t}
show_bytes "$t"

t=${s%?}
echo len=${#t}
show_bytes "$t"

t=${s%%?}
echo len=${#t}
show_bytes "$t"
