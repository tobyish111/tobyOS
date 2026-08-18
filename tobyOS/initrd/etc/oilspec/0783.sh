# WORKAROUND: wrap in bash -i -c because non-interactive bash behaves
# differently!
case $SH in
  *bash|*osh)
    $SH --rcfile /dev/null -i -c '
shopt -s extglob
fun() {
  COMPREPLY=(one two three bin)
}
compgen -X "@(two|bin)" -F fun
echo --
compgen -X "!@(two|bin)" -F fun
'
esac
