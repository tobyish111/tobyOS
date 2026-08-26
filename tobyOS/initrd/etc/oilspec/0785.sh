cd $TMP
touch spam.py spam.sh
compgen -f -- sp | sort
echo --
# WORKAROUND: wrap in bash -i -c because non-interactive bash behaves
# differently!
case $SH in
  *bash|*osh)
      $SH --rcfile /dev/null -i -c 'shopt -s extglob; compgen -f -X "!*.@(py)" -- sp'
esac
