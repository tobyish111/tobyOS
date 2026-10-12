#!/bin/sh
# timeoutcases.sh -- timeout's answers, case by case: stdout's first line,
# stderr, the status, and the time taken in tenths (tsh's `time -p` where
# date has no nanoseconds is the caller's business: T is $SECONDS-free).
# GNU coreutils' timeout (WSL's gnutimeout, through a symlink named
# timeout on PATH, LC_ALL=C) is the oracle; /bin/timeout on the guest is
# held to it.
#
#     sh timeoutcases.sh [TIMEOUT]
T=${1:-timeout}
t() {
    label=$1; shift
    "$T" "$@" > /tmp/tc.out 2> /tmp/tc.err
    printf '%s st=%s out=[%s] err=[%s]\n' "$label" "$?" \
        "$(head -1 /tmp/tc.out)" "$(tr '\n' '|' < /tmp/tc.err)"
}
t none
t onlydur 1
t okfalse 1 false
t oktrue 1 true
t exit3 1 sh -c 'exit 3'
t fires 0.3 sleep 2
t firesuf 0.005m sleep 2
t zero 0 sleep 0.2
t kill -s KILL 0.3 sleep 2
t int -s INT 0.3 sleep 2
t sigterm -s SIGTERM 0.3 sleep 2
t signum -s 9 0.3 sleep 2
t preserve --preserve-status 0.3 sleep 2
t kafter -k 0.3 0.3 sh -c 'trap "" TERM; sleep 2'
t nocmd 1 nosuchcmd
t noexec 1 /etc/passwd
t baddur abc sleep 1
t badsig -s FOO 1 true
t selfterm 1 sh -c 'kill -TERM $$'
t verbose -v 0.3 sleep 2
t group 0.3 sh -c 'sleep 2; echo after'
t bgchild 0.3 sh -c 'sleep 2 & wait'
t out 1 echo hi
t dashdash 1 -- echo hi
t opt -x 1 true
t long --bogus 1 true
t help --help
t version --version
t negdur -- -1 true
t infdur inf true
t fg --foreground 0.3 sleep 2
