# A list element whose subscript's arithmetic fails is reported, the list
# stops there -- what came before it is assigned -- and the rest of the
# line goes, status 1: `a=( x [1+]=foo y )` is a=(x). tsh failed the whole
# list, said "failed to set" too, and ran on (finding 494).
exec 2>&1
unset array
array=( a [1+]=foo b ); echo "same line"
echo "next $?"
declare -p array
f() { local -a l=( x [2*]=y z ); echo "in f $?"; }
f; echo "after f $?"
echo "next1 $?"
declare -a d=( p [3/]=q r ); echo "same $?"
echo "next2 $?"
declare -p d
