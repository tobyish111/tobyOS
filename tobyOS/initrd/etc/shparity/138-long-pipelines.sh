echo a | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat
x=$(echo s | { read v; echo "$v"1; } | { read v; echo "$v"2; } | { read v; echo "$v"3; } | { read v; echo "$v"4; } | { read v; echo "$v"5; } | { read v; echo "$v"6; } | { read v; echo "$v"7; } | { read v; echo "$v"8; } | { read v; echo "$v"9; } | { read v; echo "$v"10; }); echo "p2 [$x]"
set -o pipefail; true | true | true | true | true | true | true | true | true | false | true; echo "p3 [$?]"
echo done
