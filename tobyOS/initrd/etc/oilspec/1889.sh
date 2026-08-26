# Hm same odd behavior

show_string() {
  read s
  echo len=${#s}
  echo -n "$s" | od -A n -t x1
}

printf '.\001.' | show_string

printf '.\000.' | show_string

printf '\000' | show_string
