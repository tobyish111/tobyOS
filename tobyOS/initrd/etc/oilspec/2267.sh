case $SH in zsh) exit ;; esac  # different -x format

$SH -c -x 'echo hi'

# two flags before the command
$SH -c -x -e 'zz; true' 2> /dev/null
echo status=$?
