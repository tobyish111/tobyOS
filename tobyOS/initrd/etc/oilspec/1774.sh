cat >$TMP/rcfile <<EOF
echo RCFILE; ( echo
EOF

$SH --rcfile $TMP/rcfile -i -c 'echo flag -c'
echo status=$?
