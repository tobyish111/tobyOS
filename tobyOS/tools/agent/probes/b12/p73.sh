exec 2>&1
read z <&-; echo "1 s=$?"
mapfile m <&-; echo "2 s=$? ${#m[@]}"
x=$(cat <&-); echo "3 s=$? [$x]"
read -u 7 q; echo "4 s=$?"
exec 6<&-; read -u 6 q; echo "5 s=$?"
cat <&- 2>/dev/null; echo "6 s=$?"
while read l; do echo "l=$l"; done <&-; echo "7 s=$?"
