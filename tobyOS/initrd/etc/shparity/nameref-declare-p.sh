# declare -p of a reference prints the reference, not what it names: a
# reference to an array printed that array under the reference's name
# (finding 321).
{
a=(1 2); declare -n q=a; declare -p q
declare -A A=([k]=v); declare -n Q=A; declare -p Q
declare -n r=nothere; declare -p r
declare -n s=q; declare -p s
} 2>&1
