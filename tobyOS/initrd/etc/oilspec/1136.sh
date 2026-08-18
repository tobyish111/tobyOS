IFS='x '
check() { echo "$1" | (read a b; argv.py "$a" "$b"); }

echo '-- xs... --'
check 'x '
check 'x \ '
check 'x \ \ '
check 'x \ \ \ '
echo '-- xe... --'
check 'x\ '
check 'x\ \ '
check 'x\ \ \ '
check 'x\  '
check 'x\  '
check 'x\    '

# check 'xx\ '
# check 'xx\ '

-- xs... --
['', '']
['', '']
['', '']
['', '']
-- xe... --
['', '']
['', '']
['', '']
['', '']
['', '']
['', '']
