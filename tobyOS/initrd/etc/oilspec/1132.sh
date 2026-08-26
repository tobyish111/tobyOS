echo ' a b \' | (read a; argv.py "$a")
echo ' a b \' | (read a b; argv.py "$a" "$b")
IFS='x '
echo $'a ax  x    \\\nhello' | (read a b; argv.py "$a" "$b")
