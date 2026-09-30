declare -A m=([a]=1); m=([b]=2 [c]=3); declare -p m | cut -c1-11; echo "m [${m[b]}] [${m[c]}] ${#m[@]}"
a=(1 2); a+=(x "${a[@]}"); declare -p a
r=(q); declare -n n=r; n=("${n[@]}" z); declare -p r
declare -A k=([x]=1); k=([y]="${k[x]}"); declare -p k
acc=(); for i in 1 2 3; do acc=("${acc[@]}" "v$i"); done; declare -p acc
w=W; w=("$w" q); declare -p w
s=abc; s+=(d); declare -p s
b=(1 2 3); b=([5]=x "${b[@]}"); declare -p b
b+=([0]=y [0]+=z); declare -p b
declare -A o=([k]=1 [k]+=2 [k]+=3); declare -p o
o+=([k]+=4 [n]=5); echo "o [${o[k]}] [${o[n]}] ${#o[@]}"
c=(x); c=(); declare -p c
f() { local -a L=("${G[@]}" l); declare -p L; }; G=(g1 g2); f
x=$(q=(1 2); q=("${q[@]}" 3); echo "${q[@]}"); echo "sub [$x]"
e=(a b); e=("${e[@]:1}" "${e[0]}"); declare -p e
declare -p | grep -c $'\001lit'
echo done
