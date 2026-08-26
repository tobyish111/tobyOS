cd $TMP
printf "echo %s\n" {1..3} > tmp
HISTFILE=tmp $SH -i <<'EOF'
set +o history
echo "not recorded" >> /dev/null
set -o history
echo "recorded" >> /dev/null
EOF

case $SH in bash) echo '^D' ;; esac

grep "not recorded" tmp >> /dev/null
echo status=$?
grep "recorded" tmp >> /dev/null
echo status=$?
