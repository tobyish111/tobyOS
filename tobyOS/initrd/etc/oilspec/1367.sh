s='foo()'

case $s in
  *\(\)) echo 'match'
esac

case $SH in dash) exit ;; esac  # not implemented

shopt -s extglob

case $s in
  *(foo|bar)'()') echo 'extglob'
esac
