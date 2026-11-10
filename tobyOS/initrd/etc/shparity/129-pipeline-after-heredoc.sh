v=outer; echo inner | { read v; }; echo "h1 before [$v]"
read -r line <<EOF
body $v
EOF
echo "h2 [$line]"
v=outer; echo inner | { read v; }; echo "h3 after [$v]"
read -r a <<A; read -r b <<B
one $v
A
two $v
B
echo "h4 [$a] [$b]"
v=outer; echo inner | { read v; }; echo "h5 after two [$v]"
x=$(read -r c <<C
in capture $v
C
echo "$c"); echo "h6 [$x]"
v=outer; echo inner | { read v; }; echo "h7 after capture [$v]"
n=0; printf '%s\n' a b c | while read -r w; do n=$((n+1)); done; echo "h8 while-in-pipe n=[$n]"
echo done
