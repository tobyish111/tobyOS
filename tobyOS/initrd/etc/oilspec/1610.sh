shopt -s extglob
prefix=no
[[ --no-long-option == --@(help|verbose|$prefix-@(long|short)-'option') ]] &&
  echo TRUE
