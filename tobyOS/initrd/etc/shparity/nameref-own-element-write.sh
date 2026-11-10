# A write through a reference that comes to an element of itself: for a
# function's local reference that is the global element, which names
# nothing -- "`a[0]': not a valid identifier", an assignment error, the rest
# of the line dropped; at the top level bash removes the reference
# ("removing nameref attribute") and the element is its own (finding 357).
exec 2>&1
f() { local -n a=$1; a=X; echo after; }; a=(0); f 'a[0]'; echo "not here"
declare -p a
declare -n p=q; declare -n q='p[1]'; p=foo; echo "rc=$?"; declare -p p q
declare -n s=t; declare -n t='s[2]'; s=v; echo "[${s[2]}]"; declare -p s
