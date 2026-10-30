# An array an element was assigned to is an assigned array: emptied again
# by unset it is `declare -A A=()`, not the `declare -A A` of one only
# declared -- by an element assignment, read or a local's (finding 371).
exec 2>&1
declare -A A; A[k]=1; unset A[k]; declare -p A
declare -a a; a[1]=1; unset a[1]; declare -p a
declare -A R; read R[k] <<< v; unset R[k]; declare -p R
f() { local -A L; L[k]=1; unset L[k]; declare -p L; }; f
declare -a n; n[0]=z; declare -n n; echo "rc=$?"
