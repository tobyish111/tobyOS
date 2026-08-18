f() {
  local x=x
  tmp='' local tx=tx

  # Hm both y and ty persist in bash/zsh
  eval 'local y=y'
  tmp='' eval 'local ty=ty'

  # Why does this have an effect in OSH?  Oh because 'unset' is a special
  # builtin
  if true; then
    x='X' unset x
    tx='TX' unset tx
    y='Y' unset y
    ty='TY' unset ty
  fi

  #unset y
  #unset ty

  echo x=$x
  echo tx=$tx
  echo y=$y
  echo ty=$ty
}

f
