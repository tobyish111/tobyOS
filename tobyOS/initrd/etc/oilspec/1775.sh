$SH -c 'echo A'

cat >$TMP/rcfile <<EOF
echo 'rcfile first'
EOF

mkdir -p $TMP/rcdir

cat >$TMP/rcdir/file1 <<EOF
echo rcdir 1
EOF

cat >$TMP/rcdir/file2 <<EOF
echo rcdir 2
EOF

# --rcdir only
$SH --rcdir $TMP/rcdir -i -c 'echo B'

$SH --rcfile $TMP/rcfile --rcdir $TMP/rcdir -i -c 'echo C'
