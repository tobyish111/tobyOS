# A reference as a for loop's variable is rebound to each word, not
# written through; a word that names no variable ends the loop, "`1': not
# a valid identifier", status 1 (finding 325). tsh assigned the words to
# the first target.
{
v1=1 v2=2; declare -n v=v1; for v in v1 v2; do echo "${!v}: $v"; done; echo "after ${!v}: $v"; declare -p v
( readonly o1=1 o2=2; declare -n r=o1; for r in o1 o2; do echo "${!r} $r"; done; echo "rc=$?" )
( declare -n r=a; for r in b 1 '' c; do echo "[${!r}]"; done; echo "rc=$?"; declare -p r )
( declare -n q=a; readonly q; for q in b c; do echo "${!q}"; done; echo "rc=$?" )
f() { local -n l=a; for l in x y; do echo "${!l}"; done; }; f
} 2>&1
