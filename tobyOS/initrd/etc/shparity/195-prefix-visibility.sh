# A PREFIX ASSIGNMENT is visible to the assignment words after it and to the
# command itself -- not to the command's own words, which bash expands first.
# tsh let every word see it: `A4=w echo "[$A4]"` printed w.
exec 2>&1
A1=x readonly v2=$A1; echo "1 [$v2]"
A2=y export v3=$A2; echo "2 [$v3]"
A3=z declare v4=$A3; echo "3 [$v4]"
A4=w echo "4 [$A4]"
A5=q B5=$A5 env | grep ^B5
A6=r B6=[$A6] : ; echo "6 [$B6] [$A6]"
A7=s eval echo "7 [\$A7]"
f() { echo "9 [$1] [$A9]"; }; A9=u f "$A9"
