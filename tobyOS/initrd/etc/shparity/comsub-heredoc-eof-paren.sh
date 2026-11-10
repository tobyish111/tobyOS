# A here-document inside a $( ) that `EOF)` ends -- the delimiter, then the
# substitution's `)` -- is "delimited by end-of-file": bash warns, and the
# body is what came before (finding 448).
exec 2>&1
text=$(cat <<EOF
here is the text
EOF)
echo = $text =
unbalanced=$(cat <<EOF
this paren ) is not a problem
EOF)
echo $unbalanced
