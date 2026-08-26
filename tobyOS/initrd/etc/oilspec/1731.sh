# A space between 0 and <<EOF causes it to pass '0' as an arg to cat.
cat 0<<EOF
one
EOF
