declare -A a
a=([k2]=-{a,b}-)
echo ${a["k2"]}
