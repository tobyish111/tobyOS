# Associative arrays: string keys, declare -A required.
declare -A m
m[alpha]=1
m[beta]=2
m["two words"]=3
echo "a=${m[alpha]} b=${m[beta]} tw=${m[two words]}"
echo "n=${#m[@]}"
declare -A n2=([k1]=v1 [k2]=v2)
echo "n2=${n2[k1]},${n2[k2]} count=${#n2[@]}"
unset 'm[beta]'
echo "after-unset n=${#m[@]} beta=[${m[beta]}]"
echo end
