case $SH in dash|mksh|zsh|ash) exit ;; esac

{ 
  printf '.\000.\n'
  printf '.\000.\n'
} |
{ mapfile LINES
  echo len=${#LINES[@]}
  for line in ${LINES[@]}; do
    echo -n "$line" | od -A n -t x1
  done
}

# bash is INCONSISTENT:
# - it TRUNCATES at \0, with 'mapfile'
# - rather than just IGNORING \0, with 'read'
