# A here-document's own \001 byte is text, and so is one an expansion put
# there: tsh took the written one for a field mark and dropped it, and
# gave the expanded one with its escape byte in front (finding 502).
exec 2>&1
f=${TMP:-/tmp}/hdcb.$$
printf 'cat <<EOF\na\001b\n${none-c\001d}\nEOF\n' > "$f"
. "$f" | od -c | head -2
rm -f "$f"
x=$'p\001q'
cat <<EOF | od -c | head -1
$x
EOF
read -r y <<EOF
$x
EOF
printf %s "$y" | od -c | head -1
