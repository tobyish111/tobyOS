IFS='x '
echo x     | (read a b; argv.py "$a" "$b")
echo xx    | (read a b; argv.py "$a" "$b")
echo xxx   | (read a b; argv.py "$a" "$b")
echo xxxx  | (read a b; argv.py "$a" "$b")
echo xxxxx | (read a b; argv.py "$a" "$b")
echo '-- spaces --'
echo 'x    ' | (read a b; argv.py "$a" "$b")
echo 'xx   ' | (read a b; argv.py "$a" "$b")
echo 'xxx  ' | (read a b; argv.py "$a" "$b")
echo 'xxxx ' | (read a b; argv.py "$a" "$b")
echo 'xxxxx' | (read a b; argv.py "$a" "$b")
echo '-- with char --'
echo 'xa    ' | (read a b; argv.py "$a" "$b")
echo 'xax   ' | (read a b; argv.py "$a" "$b")
echo 'xaxx  ' | (read a b; argv.py "$a" "$b")
echo 'xaxxx ' | (read a b; argv.py "$a" "$b")
echo 'xaxxxx' | (read a b; argv.py "$a" "$b")
['', '']
['', 'x']
['', 'xx']
['', 'xxx']
['', 'xxxx']
-- spaces --
['', '']
['', 'x']
['', 'xx']
['', 'xxx']
['', 'xxxx']
-- with char --
['', 'a']
['', 'ax']
['', 'axx']
['', 'axxx']
['', 'axxxx']
