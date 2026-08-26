a1=()
a2=("")
a3=("" "")

echo "a1 unset: [${a1[@]-unset}]"
echo "a1 empty: [${a1[@]:-empty}]"
echo "a2 unset: [${a2[@]-unset}]"
echo "a2 empty: [${a2[@]:-empty}]"
echo "a3 unset: [${a3[@]-unset}]"
echo "a3 empty: [${a3[@]:-empty}]"
