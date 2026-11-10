IFS=':%'
a=(x\:y); echo "${#a[@]} ${a[0]}"
a=(ham\ cheese\:colon); echo "${#a[@]} ${a[0]}"
a=('p:q' "r:s"); echo "${#a[@]} ${a[0]} ${a[1]}"
set -- x\:y; echo "$# $1"
for w in x\:y; do echo "for $w"; done
