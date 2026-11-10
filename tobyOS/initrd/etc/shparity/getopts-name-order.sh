# getopts says "illegal option" before it sets NAME, so that message comes
# first when NAME cannot be set; and a NAME it cannot set is status 2 only
# when it is readonly, 1 otherwise -- through a reference with no target
# (finding 332). tsh said them the other way round and gave 2 for both.
{ ( readonly o=keep; getopts ab o -z; echo "rc=$?" ); } 2>&1
{ ( declare -n r; getopts x r -h; echo "rc=$?" ); } 2>&1
{ ( readonly o=keep; getopts :b: o -b; echo "rc=$?" ); } 2>&1
