cat > snork << 'EOF'
echo hello $BLAH
EOF

chmod +x snork
$SH -c 'BLAH=123; ./snork'
$SH -c 'BLAH=123; exec ./snork'
$SH -c 'BLAH=123 exec ./snork'
