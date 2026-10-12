#!/bin/sh
# timeoutcases4.sh -- signals sent TO timeout while its command runs: GNU's
# passes a HUP INT QUIT or TERM on to the command (and its group), and -k's
# clock starts from that signal as from a timeout. The status after each.
#
#     sh timeoutcases4.sh [TIMEOUT]
T=${1:-timeout}
x() {
    label=$1 sig=$2; shift 2
    "$T" "$@" > /tmp/tc.out 2> /tmp/tc.err &
    p=$!
    sleep 0.3
    kill -"$sig" "$p"
    wait "$p"
    printf '%s st=%s out=[%s] err=[%s]\n' "$label" "$?" \
        "$(head -1 /tmp/tc.out)" "$(tr '\n' '|' < /tmp/tc.err)"
}
x extterm TERM 5 sleep 3
x extint INT 5 sleep 3
x exthup HUP 5 sleep 3
x extquit QUIT 5 sleep 3
x extusr1 USR1 5 sleep 3
x extgroup TERM 5 sh -c 'sleep 3; echo after'
x extcaught TERM 5 sh -c 'trap "echo caught; exit 7" TERM; sleep 3 & wait'
x extkafter TERM -k 0.5 5 sh -c 'trap "" TERM; sleep 3'
x extfg TERM -f 5 sh -c 'sleep 3; echo after'
x extverbose TERM -v 5 sleep 3
x extsigint TERM -s INT 5 sleep 3
