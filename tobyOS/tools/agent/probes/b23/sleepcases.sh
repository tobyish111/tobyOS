#!/bin/sh
# sleepcases.sh -- sleep's answers, case by case: its stdout, stderr and
# exit status for each argument list, and whether it took about as long as
# asked (in tenths of a second, rounded). Run with the sleep to measure
# first on PATH -- GNU coreutils' in WSL is the oracle; /bin/sleep on the
# guest is the one held to it.
#
#     sh sleepcases.sh [SLEEP]
S=${1:-sleep}
t() {
    # t LABEL ARGS... -- run, report out/err/status and the time it took
    label=$1; shift
    a=$(date +%s%N 2>/dev/null)
    "$S" "$@" > /tmp/sc.out 2> /tmp/sc.err
    st=$?
    b=$(date +%s%N 2>/dev/null)
    d=$(( (b - a + 50000000) / 100000000 ))
    printf '%s st=%s t=%s out=[%s] err=[%s]\n' "$label" "$st" "$d" \
        "$(cat /tmp/sc.out | head -1)" "$(tr '\n' '|' < /tmp/sc.err)"
}
t none
t abc abc
t empty ''
t half 0.5
t dotfive .5
t fivedot 0.
t s 0.3s
t m 0.005m
t h 0.0001h
t d 0d
t sum 0.2 0.3
t exp 1e-1
t hex 0x0.8
t plus +0.2
t space ' 0.2'
t trail '0.2 '
t neg -1
t negdot -0.5
t dashdash -- 0.2
t bad2 0 abc def
t bigsuffix 0.2S
t xs 1x
t nan nan
t ms 0.2ms
t opt -x
t long --bogus
t help --help
t version --version
# an endless sleep: still sleeping when timeout ends it (124)
for a in inf infinity INF 'inf s'; do
    timeout 1 "$S" "$a" 2>/tmp/sc.err; printf 'inf[%s] st=%s err=[%s]\n' "$a" "$?" "$(tr '\n' '|' < /tmp/sc.err)"
done
