exec 2>&1
a=(1)
cat <<E; echo "1 s=$?"
${a[]}
E
cat <<E && echo "2 and" || echo "2 or s=$?"
x${a[]}y
E
{ cat <<E; echo "3 in s=$?"; } ; echo "3 out"
${a[]}
E
f() { cat <<E
${a[]}
E
echo "4 f s=$?"; }
f; echo "4 after s=$?"
cat <<E
${u:?boom}
E
echo "5 s=$?"
x=$(cat <<E
${a[]}
E
); echo "6 s=$? [$x]"
cat <<E
$(( 1/0 ))
E
echo "7 s=$?"
cat <<E
ok ${a[0]}
E
echo "8 end"
