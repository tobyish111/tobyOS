IFS='x '
echo 'x\  \ ' | (read a b; argv.py "$a" "$b")
['', '  ']
['', '']
['', ' ']
