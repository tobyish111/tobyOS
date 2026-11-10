echo x | read v; echo "v=[$v]"
echo a b | read -a arr; declare -p arr 2>&1
f() { g=(1 2); h=3; }; echo q | f; declare -p g 2>&1; echo "h=[$h]"
echo q | mapfile -t m; declare -p m 2>&1
