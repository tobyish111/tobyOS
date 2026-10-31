# Under functrace the DEBUG and RETURN traps work in a ( ) and a $( ), as
# in a function -- tsh's were only shown there (finding 535).
exec 2>&1
f() { :; }
trap 'echo "R ${FUNCNAME[0]-}"' RETURN
trap 'echo "D $LINENO"' DEBUG
set -T
( echo a )
x=$(f); echo "x=[$x]"
set +T
( echo c )
x=$(f); echo "x=[$x]"
trap - DEBUG RETURN
