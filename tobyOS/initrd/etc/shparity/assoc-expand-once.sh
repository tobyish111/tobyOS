# Under assoc_expand_once an associative key is taken as it is, not
# expanded again: read and printf -v into `A[$k]` with k="8's" or k='$v'
# set those keys; declare's subscript ends at its first `]`; arithmetic
# removes a key's quotes but runs nothing in it (finding 368). tsh had the
# option and never read it.
exec 2>&1
shopt -s assoc_expand_once
declare -A A; v=Z; for k in '$v' "8's" 'a]b' '`' '[a'; do read A[$k] <<< r; printf -v A[$k] %s p; done; declare -p A
declare -A D; for k in 'x[y' 'a]b' "8's"; do declare D[$k]=1; echo "[$k] $?"; done; declare -p D
declare -A N; b="80's"; ((++N[$b])); let "++N[$b]"; x='$(echo ran; echo K)'; N[$x]=9; N[x]=2
echo "$((1+N[$x])) $(( N['x'] ))"; declare -p N
