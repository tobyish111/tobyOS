# A ( ) whose body is one simple command, and a coprocess that is one,
# exec that command even when it has redirections of its own -- the process
# becomes the program -- where the last command of a list with one of its
# own is forked. tsh forked for any redirection (finding 640). Each line
# waits up to a second for the child to become `sleep`.
as() {
    local c i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        c=$(tr '\0' ' ' < /proc/$1/cmdline)
        case $c in sleep*|cat*) break ;; esac
        sleep 0.1
    done
    echo "${c%% *}" | sed 's|.*/||; s/^bash$/SH/; s/^tsh$/SH/; s/^sh$/SH/'
}
( sleep 2 > /dev/null ) & as $!; wait
( sleep 2 2> /dev/null ) & as $!; wait
( sleep 2 ) > /dev/null & as $!; wait
( sleep 2 < /dev/null ) & as $!; wait
coproc sleep 2; as $COPROC_PID; wait
coproc cat > /dev/null; as $COPROC_PID; eval "exec ${COPROC[1]}>&-"; wait
( : ; sleep 2 > /dev/null ) & as $!; wait
