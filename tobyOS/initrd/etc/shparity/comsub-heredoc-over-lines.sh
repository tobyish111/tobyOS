# A here-document inside a $( ) that runs over lines is read with the
# substitution, where `EOF)` and a `)` delimiter's `))` end it -- tsh read
# the body while joining the lines, wanted a line EOF, and ran to the end of
# the file (finding 450).
exec 2>&1
echo $(
cat <<\EOF
hi
EOF)
echo $(
cat <<\)
there
))
x=$(
cat <<EOF
a $((1+2)) b
EOF
)
echo "$x"
