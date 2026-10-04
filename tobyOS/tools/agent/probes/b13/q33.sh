exec 2>&1 </dev/null
mkdir -p /tmp/q33d && cd /tmp/q33d && rm -f * && touch ax bx 'a*x' c
w='a*x'
printf '1[%s]\n' "$w"
for w in 'a*x'; do printf '2[%s]\n' "$w"; done
set -- 'a*x'; for w; do printf '3[%s]\n' "$w"; done
f() { for w; do printf '4[%s]\n' "$w"; done; }; f 'a*x'
f2() { printf '5[%s]\n' "$1"; }; f2 'a*x'
f3() { printf '6%d\n' $#; }; f3 *"x"
echo "7" "$w"
cd /; rm -rf /tmp/q33d
echo end
