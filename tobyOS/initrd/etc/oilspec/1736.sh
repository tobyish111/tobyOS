echo hello >$TMP/hello.txt

cat <<EOF <$TMP/hello.txt
here
EOF
