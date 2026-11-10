echo 'hello world  test   ' | (read a b; argv.py "$a" "$b")
echo '-- IFS=x --'
IFS='x '
echo 'a ax  x  '     | (read a b; argv.py "$a" "$b")
echo 'a ax  x  x'    | (read a b; argv.py "$a" "$b")
echo 'a ax  x  x  '  | (read a b; argv.py "$a" "$b")
echo 'a ax  x  x  a' | (read a b; argv.py "$a" "$b")
