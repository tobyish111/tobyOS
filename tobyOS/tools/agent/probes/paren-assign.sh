x='(a b)'; declare -p x
x2="(a b)"; declare -p x2
x3=\(a\ b\); declare -p x3
read z <<< "(c d)" ; declare -p z
z2=$(echo "(e f)"); declare -p z2
w="(g h)"; w2=$w; declare -p w2
w3="$w"; declare -p w3
export e1=$w; declare -p e1
declare d1=$w; declare -p d1
local_t() { local l1=$w; declare -p l1; }; local_t
q=$w env | grep '^q='
