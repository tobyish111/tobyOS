# http://landley.net/notes.html#20-06-2020

# temp binding
readonly abc=123
abc=def echo one
echo status=$?

echo potato < /does/not/exist || echo hello
