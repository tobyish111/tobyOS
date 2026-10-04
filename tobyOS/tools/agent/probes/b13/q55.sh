exec 2>&1 </dev/null
cd /tmp
f=/tmp/q55.$$.sh
r() { printf "$1" > $f; $SH $f 2>&1 | sed "s#$f#SCRIPT#" | od -An -c | tr -s ' '; echo "-- rc=${PIPESTATUS[0]}"; }
r 'echo a\r\necho b\n'
r 'echo a\rb c\n'
r 'x=1\r\necho "[$x]"\n'
r 'if true\r\nthen echo y\r\nfi\r\n'
r 'f() {\r\necho in\r\n}\r\nf\r\n'
r 'cat <<E\r\nx\r\nE\r\nE\n'
r '# c\r\necho z\n'
r 'case a in\r\na) echo m;;\r\nesac\r\n'
r 'echo a\\\r\nb\n'
r 'for i in 1 2\r\ndo echo $i\r\ndone\r\n'
r 'echo "a\r\nb"\n'
r "echo 'a\r'\n"
r 'a=(1\r 2)\necho ${#a[@]}\n'
r 'echo $((1\r+1))\n'
r '[[ a\r == a ]] && echo eq || echo ne\n'
r 'x=a\r; echo ${#x}\n'
$SH -c "$(printf 'echo a\r')" | od -An -c | tr -s ' '
$SH -c "$(printf 'eval \"echo q\rw\"')" | od -An -c | tr -s ' '
printf 'a\r\n' | $SH -c 'read x; echo ${#x}'
printf 'a\r\n' | $SH -c 'x=$(cat); echo ${#x}'
printf 'echo s\r\n' > $f; $SH -c ". $f" | od -An -c | tr -s ' '
rm -f $f
echo end
