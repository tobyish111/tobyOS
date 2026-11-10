# The line a simple command is on, when a backslash-newline continues it,
# is where bash's reader was when it took the command: the line of the
# character that ends its second word, or of what follows the first word
# when there is no second -- `nosuch a\` and `   b` is line 2, `nosuch \`
# and an empty line is line 2, and so is `x=1\` with the command on the
# next line; tsh named the first (finding 590).
f=$(mktemp)
cat > "$f" <<'EOF'
nosuchlc1 a\
   b
nosuchlc2 $(echo x)\
   b
nosuchlc3 a \
   b
nosuchlc4 \

nosuchlc5\
;:
x=1\
 nosuchlc6
echo "end $LINENO"
EOF
"$BASH" "$f" 2>&1 | sed 's/^.*: line /line /'
rm -f "$f"
