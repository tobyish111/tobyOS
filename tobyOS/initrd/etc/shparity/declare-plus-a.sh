# declare +a or +A of an array is bash's "cannot destroy array variables in
# this way", status 1 (finding 354). tsh said nothing.
{
declare -A c2=([a]=1); declare +A c2; echo "rc=$?"; declare -p c2
declare -a c3=(1); declare +a c3; echo "rc=$?"; declare -p c3
s=1; declare +a s; echo "rc=$?"; declare -p s
} 2>&1
