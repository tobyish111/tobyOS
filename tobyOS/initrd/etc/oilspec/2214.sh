case $SH in dash) exit ;; esac
case $SH in bash|*osh) flag='--rcfile /dev/null' ;; esac

code='test -o emacs; echo $?; test -o vi; echo $?'

echo non-interactive
$SH $flag -c "$code"

echo interactive
$SH $flag -i -c "$code"
