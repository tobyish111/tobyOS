show_bytes() {
  echo -n "$1" | od -A n -t x1
}

s=$(printf '\000x')
echo len=${#s}
show_bytes "$s"

# strip one char from the front
s=${s#?}
echo len=${#s}
show_bytes "$s"

echo ---

s=$(printf '\001x')
echo len=${#s}
show_bytes "$s"

# strip one char from the front
s=${s#?}
echo len=${#s}
show_bytes "$s"
