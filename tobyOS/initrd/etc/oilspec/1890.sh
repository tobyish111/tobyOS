case $SH in dash) exit ;; esac

show_string() {
  read -n 3 s
  echo len=${#s}
  echo -n "$s" | od -A n -t x1
}


printf '.\001.' | show_string

printf '.\000.' | show_string

printf '\000' | show_string


len=3
 2e 01 2e
len=1
 2e
len=0
