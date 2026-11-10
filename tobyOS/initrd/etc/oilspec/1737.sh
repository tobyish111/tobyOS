echo hello >$TMP/hello.txt

cat <$TMP/hello.txt <<EOF
here
EOF
