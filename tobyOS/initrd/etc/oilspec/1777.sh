$SH -c 'echo A'

cat >$TMP/rcfile <<EOF
echo rcfile
EOF

mkdir -p $TMP/rcdir
cat >$TMP/rcdir/file1 <<EOF
echo rcdir 1
EOF

cat >$TMP/rcdir/file2 <<EOF
echo rcdir 2
EOF

$SH --norc --rcfile $TMP/rcfile -c 'echo C'
case $SH in bash) exit ;; esac

$SH --norc --rcfile $TMP/rcfile --rcdir $TMP/rcdir -c 'echo D'
