# A here-document's text takes `$'...'` as the bytes written, as double
# quotes do, and so does a ${x-WORD} word in it; only a pattern operator's
# word decodes it -- tsh decoded every one (finding 476).
exec 2>&1
x=notOK
cat <<EOF
a: $'not'
b: $'a\tb\nc'
c: ${x#$'not'}
d: "$'q'" '$'r''
e: ${zz-$'a\tb'}
EOF
cat <<'EOF'
f: $'raw'
EOF
