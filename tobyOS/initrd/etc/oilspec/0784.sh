# WORKAROUND: wrap in bash -i -c because non-interactive bash behaves
# differently!
case $SH in
  *bash|*osh)
      $SH --rcfile /dev/null -i -c 'shopt -s extglob; compgen -X "@(two|bin)" -W "one two three bin"'
esac
