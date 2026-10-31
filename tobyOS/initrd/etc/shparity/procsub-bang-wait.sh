# A process substitution's process is $!, and `wait "$!"` gives its
# status -- tsh left $! alone and wait had nothing (finding 519).
exec 2>&1
cat <(exit 123) >/dev/null
wait "$!"
echo $?
cat <(sleep 0.3; exit 7) >/dev/null
wait $!; echo $?
