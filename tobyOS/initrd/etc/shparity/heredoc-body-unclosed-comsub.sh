# A $( ) in a here-document's body that never closes is bash's parser error
# for that text, on the line its reader has reached -- the body's
# delimiter: "command substitution: line 3: unexpected EOF while looking
# for matching `)'"; tsh said "bad command substitution" (finding 594).
f=$(mktemp)
printf 'read foo <<EOF\n$(seq 10\nEOF\n\necho "[$foo] $?"\n' > "$f"
"$BASH" "$f" 2>&1 | sed 's/^[^ ]*: command/command/'
printf ':\nread foo <<EOF\nline\n$(echo x\nEOF\necho "[$foo]"\n' > "$f"
"$BASH" "$f" 2>&1 | sed 's/^[^ ]*: command/command/'
rm -f "$f"
