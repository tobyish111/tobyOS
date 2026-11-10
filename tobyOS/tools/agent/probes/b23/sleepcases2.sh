#!/bin/sh
# sleepcases2.sh -- sleep's edges: options after operands, abbreviated long
# options, a negative after --, numbers strtod reads oddly, overflow. Same
# report as sleepcases.sh; an endless sleep shows st=124.
#
#     sh sleepcases2.sh [SLEEP]
S=${1:-sleep}
t() {
    label=$1; shift
    timeout 2 "$S" "$@" > /tmp/sc.out 2> /tmp/sc.err
    printf '%s st=%s out=[%s] err=[%s]\n' "$label" "$?" \
        "$(head -1 /tmp/sc.out)" "$(tr '\n' '|' < /tmp/sc.err)"
}
t helpafter 0 --help
t optafter 0 -x
t verafter 0.1 --version
t negafter -- -1
t negsum 1 -- -1
t he --he
t ver --ver
t h -h
t v -v
t xend 0x
t eend 1e
t twodots 1.5.5
t spacesuf '0.1 s'
t twosuf 0.1sm
t blank ' '
t tab '	0.1'
t over 1e400
t under 1e-400
t zero 0
t zeros 0s 0m 0h 0d
t leadzero 00.1
t comma 0,1
t posinf +inf
t dashx -- -x
