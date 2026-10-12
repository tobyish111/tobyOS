#!/bin/sh
# timeoutcases2.sh -- timeout's option and signal-name forms. Same report as
# timeoutcases.sh.
#
#     sh timeoutcases2.sh [TIMEOUT]
T=${1:-timeout}
t() {
    label=$1; shift
    "$T" "$@" > /tmp/tc.out 2> /tmp/tc.err
    printf '%s st=%s out=[%s] err=[%s]\n' "$label" "$?" \
        "$(head -1 /tmp/tc.out)" "$(tr '\n' '|' < /tmp/tc.err)"
}
t lower -s kill 0.3 sleep 2
t sigprefix -s SIGINT 0.3 sleep 2
t lowersig -s sigint 0.3 sleep 2
t num15 -s 15 0.3 sleep 2
t num99 -s 99 0.3 sleep 2
t num0 -s 0 0.3 sleep 2
t longsig --signal=INT 0.3 sleep 2
t longsigsp --signal INT 0.3 sleep 2
t abbr --sig=INT 0.3 sleep 2
t combined -vsINT 0.3 sleep 2
t pshort -p 0.3 sleep 2
t fshort -f 0.3 sleep 2
t kbad -k abc 0.3 true
t kzero -k 0 0.3 sleep 2
t kverbose -v -k 0.3 0.3 sh -c 'trap "" TERM; sleep 2'
t pkill -p -s KILL 0.3 sleep 2
t fkill -f -s KILL 0.3 sleep 2
t fkafter -f -k 0.3 0.3 sh -c 'trap "" TERM; sleep 2'
t hup -s HUP 0.3 sleep 2
t usr1 -s USR1 0.3 sleep 2
t ignterm 0.3 sh -c 'trap "" TERM; sleep 1'
t nonnum 1x true
t mstime 0.3s sleep 2
t dsuf 0.000004d sleep 2
t sigabbr -s TER 0.3 sleep 2
