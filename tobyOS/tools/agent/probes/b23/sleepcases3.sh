#!/bin/sh
# sleepcases3.sh -- the last of sleep's edges: an option's argument, a lone
# dash, negative zero, inf with a suffix, hex forms. Same report as
# sleepcases2.sh.
#
#     sh sleepcases3.sh [SLEEP]
S=${1:-sleep}
t() {
    label=$1; shift
    timeout 2 "$S" "$@" > /tmp/sc.out 2> /tmp/sc.err
    printf '%s st=%s out=[%s] err=[%s]\n' "$label" "$?" \
        "$(head -1 /tmp/sc.out)" "$(tr '\n' '|' < /tmp/sc.err)"
}
t helparg --help=x
t verarg --version=1
t amb --
t dash -
t negzero -- -0
t negzeros -- -0.0s
t neginf -- -inf
t infs infs
t infin infin
t infm 1 infm
t hexp 0x1p-1
t hexdot 0x.8
t hexcap 0X0.1P+2
t hexnop 0x1p
t dbl --help --version
t badthenhelp --bogus --help
t xy -xy
t bigint 99999999999999999999999
t tiny 0.0000000001
t many 0.1 0.1 0.1
