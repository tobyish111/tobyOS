mkdir -p $TMP/bin
echo "echo hello" > $TMP/bin/hello
chmod +x $TMP/bin/hello
export PATH=$TMP/bin
command -p hello
