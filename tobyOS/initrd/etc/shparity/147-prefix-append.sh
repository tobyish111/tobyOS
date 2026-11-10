A=a; A+=b true; echo "1 [$A]"
A=a; A+=b sh -c 'echo "2 [$A]"'; echo "3 [$A]"
A=a; export A; A+=b env | grep '^A='; echo "4 [$A]"
B=x; B+=y C=z eval 'echo "5 [$B$C]"'; echo "6 [$B] [${C-unset}]"
f() { echo "7 [$A]"; }; A=q; A+=r f; echo "8 [$A]"
echo done
