# The (( and $(( cases of the syntax-error probe, one at a time.
exec 2>&1
t() { eval "$1"; echo "  [$1] status=$?"; }
t 'echo $((1+'
echo next1
t '((1+'
echo next2
t 'x=1; ((x+'
echo done
