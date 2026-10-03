cd /tmp || exit 1
rm -f p110a p110b
{ echo out; echo err >&2; } 1>&p110a; echo "1 rc=$? [$(tr '\n' ' ' < p110a)]"
{ echo out; echo err >&2; } >&p110b; echo "2 rc=$? [$(tr '\n' ' ' < p110b)]"
echo hi 2>&p110c; echo "3 rc=$?"; ls p110c 2>&1 | sed 's/^/3 ls: /'
echo hi 3>&p110d; echo "4 rc=$?"
x=-; echo hi >&"$x"; echo "5 rc=$?"
z=7-; echo hi >&$z; echo "6 rc=$?"
rm -f p110a p110b p110c p110d
