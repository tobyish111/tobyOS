a1=()
a2=("" "")
a3=(foo bar)

echo "$a1, ${a1-(unset)}, ${a1:-(empty)};"
echo "$a2, ${a2-(unset)}, ${a2:-(empty)};"
echo "$a3, ${a3-(unset)}, ${a3:-(empty)};"
