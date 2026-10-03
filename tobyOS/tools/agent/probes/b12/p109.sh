cd /tmp || exit 1
x=
read -r a <&"$x"; echo "1 rc=$?"
read -r a <&""; echo "2 rc=$?"
cat <&foo; echo "3 rc=$?"
echo hi >&"$x"; echo "4 rc=$?"
echo hi 1>&""; echo "5 rc=$?"
echo hi >&p109out.txt; echo "6 rc=$? $(cat p109out.txt)"; rm -f p109out.txt
echo hi 1>&p109out2.txt; echo "7 rc=$? $(cat p109out2.txt 2>&1)"; rm -f p109out2.txt
cat 0<&/etc/hostname >/dev/null; echo "8 rc=$?"
y="1 2"; echo hi >&$y; echo "9 rc=$?"
echo hi 2>&"$x"; echo "10 rc=$?"
exec 3<&""; echo "11 rc=$?"
