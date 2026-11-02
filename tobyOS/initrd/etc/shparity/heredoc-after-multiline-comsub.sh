# A here-document whose operator comes before a $( ) that runs over lines
# has its body after the line where the $( ) closes, as bash reads the $( )
# whole first: the lines that look like the body are the $( )'s commands,
# numbered on from that line -- tsh took the body from the line after the
# first (finding 593).
f=$(mktemp)
cat > "$f" <<'EOF'
cat <<END && echo $(
nosuchhd1
END
echo two)
body line
END
cat <<A && cat <<B && echo $(
echo three)
in-a
A
in-b
B
EOF
"$BASH" "$f" 2>&1 | sed 's/^[^ ]*: line /line /'
rm -f "$f"
