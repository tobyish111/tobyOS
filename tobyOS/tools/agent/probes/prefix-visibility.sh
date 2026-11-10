A1=x readonly v2=$A1; echo "1 [$v2]"
A2=y export v3=$A2; echo "2 [$v3]"
A3=z declare v4=$A3; echo "3 [$v4]"
A4=w echo "4 [$A4]"
A5=q B5=$A5 env | grep ^B5
A6=r B6=[$A6] : ; echo "6 [$B6] [$A6]"
A7=s eval echo "7 [\$A7]"
A8=t local v8=$A8 2>/dev/null; echo "8 [$v8]"
f() { echo "9 [$1] [$A9]"; }; A9=u f "$A9"
