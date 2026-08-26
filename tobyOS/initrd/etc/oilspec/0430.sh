f() {
  local "$1"  # Only x is assigned here
  echo x=\'$x\'
  echo a=\'$a\'

  local $1  # x and a are assigned here
  echo x=\'$x\'
  echo a=\'$a\'
}
f 'x=y a=b'
# osh and zsh don't do word splitting
