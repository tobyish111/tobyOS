# A function that makes OPTIND local gives getopts' place back at its
# return, as it was when the function was called -- inside a word too: the
# caller's loop over -xyz goes on at z (finding 315). tsh gave back OPTIND's
# number only, and the caller started its word over and looped for good.
f() { local OPTIND=1 o; while getopts :abcxyz o "$@"; do echo "o=$o $OPTIND"; [ "$o" = y ] && f -ab; done; echo "end $OPTIND"; }
f -xyz -c
g() { local OPTIND; while getopts pq o; do echo "g $o"; done; echo "g end $OPTIND"; }
h() { local OPTIND=1 o; while getopts :abc o "$@"; do echo "h $o"; [ "$o" = a ] && g -p -q; done; echo "h end $OPTIND"; }
h -abc
# the place at the call, not at the `local`; a callee's local; one made in eval
k() { getopts abc o -abc; echo "k $o"; local OPTIND; getopts xy o -xy; echo "k2 $o"; }
getopts abc o -abc; echo "top $o"; k; getopts abc o -abc; echo "top2 $o $OPTIND"
m() { local OPTIND; getopts xy o -xy; echo "m $o"; }
n() { getopts abcd o -abcd; echo "n $o"; m; getopts abcd o -abcd; echo "n2 $o"; }
OPTIND=1; getopts abcd o -abcd; echo "top $o"; n; getopts abcd o -abcd; echo "top2 $o"
e() { eval 'local OPTIND'; getopts xy o -xy; }
OPTIND=1; getopts abc o -abc; e; getopts abc o -abc; echo "eval $o"
# an unset of the caller's local OPTIND starts the scan over
u() { unset OPTIND; while getopts pq o -pq; do echo "u $o"; done; }
v() { local OPTIND; while getopts abc o -abc; do echo "v $o"; [ "$o" = a ] && u; done; echo "v end $OPTIND"; }
v
# a subshell given a word its place is at the end of starts that word over
OPTIND=1; while getopts abc o -abc; do echo "$o"; ( getopts pq x -pq; echo "sub $x $OPTIND" ); done
# a readonly OPTIND: the scan goes on from getopts' own place
( readonly OPTIND; while getopts ab:c o -a -b x -c y; do echo "$o ${OPTARG-} $OPTIND"; done; echo "end rc=$? $OPTIND" ) 2>&1
