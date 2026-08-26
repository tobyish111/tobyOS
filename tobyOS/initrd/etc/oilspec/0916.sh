$SH --norc -i <<'EOF'
echo 'foo' > /dev/null
echo 'bar' > /dev/null
history -c 
history | wc -l
EOF

case $SH in bash) echo '^D' ;; esac
