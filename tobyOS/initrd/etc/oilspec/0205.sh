declare -A A=([k1]=v [k2]=-{a,b}-)
echo ${A["k1"]}
echo ${A["k2"]}
