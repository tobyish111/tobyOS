# TODO: should we change this?  We're not compatible with bash or busybox ash

trap 'echo bye' EXIT

# NOT a subshell
trap > traps.txt
wc -l traps.txt

echo '( )'
( trap )

echo '$(trap)'
echo $(trap)

echo 'trap | cat'
trap | cat

1 traps.txt
( )
$(trap)
trap -- 'echo bye' EXIT
trap | cat
bye
