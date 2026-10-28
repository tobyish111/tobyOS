# `. /dev/stdin` reads what the builtin's fd 0 is -- in a pipeline the
# pipe -- as bash's does. tsh kept a builtin's pipe end in its own fd
# table, and opening the path reached the process's fd 0: it waited there
# (bash's source.tests; finding 291).
echo 'echo three - OK' | . /dev/stdin
echo "rc=$?"
echo 'echo "args: $1 $2"' | . /dev/stdin p q
printf 'echo before; return 3; echo after\n' | . /dev/stdin; echo "rc=$?"
printf 'echo redirected\n' > ./src.in; . /dev/stdin < ./src.in; rm -f ./src.in
exec 3< <(echo "echo via3")
. /dev/fd/3
