echo 'Aa b \ a\ b' | (read a b; argv.py "$a" "$b")
echo 'Aa b \ a\ b' | (read a b c; argv.py "$a" "$b" "$c")
echo 'Aa b \ a\ b' | (read a b c d; argv.py "$a" "$b" "$c" "$d")
