cd $TMP
mkdir -p test-no-shebang
echo 'echo hi' > test-no-shebang/script
chmod +x test-no-shebang/script
"$SH" -c 'test-no-shebang/script'
