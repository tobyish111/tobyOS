exec 2>&1
select v in alpha beta gamma; do echo "v=[$v] R=[$REPLY]"; break; done <<< 2
echo "s=$?"
select v in a b; do echo "v=[$v] R=[$REPLY]"; done <<'EOF'
9
x

  1
2
EOF
echo "s=$?"
PS3='pick> '
select v in one two; do echo "v=$v"; [ -n "$v" ] && break; done <<< 'zz
2'
set -- p1 'p 2'
select v; do echo "v=[$v]"; break; done <<< 2
select v in; do echo never; done; echo "empty s=$?"
COLUMNS=40
select v in aaaaaaaaaa bbbbbbbbbb cccccccccc dddddddddd eeeeeeeeee ffffffffff gggggggggg hhhhhhhhhh iiiiiiiiii jjjjjjjjjj kkkkkkkkkk; do break; done <<< 1
unset COLUMNS
select v in $(seq 1 25); do echo "v=$v"; break; done <<< 25
select v in a b; do echo "c v=$v"; continue; done <<< '1
2'
echo "done s=$?"
f() { select v in x y; do return 7; done <<< 1; }; f; echo "f=$?"
readonly ro=1; select ro in a; do echo body; done <<< 1; echo "ro s=$?"
