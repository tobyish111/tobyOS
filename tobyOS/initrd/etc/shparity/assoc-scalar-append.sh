# `name+=word` on an associative array appends to its element 0, the KEY
# "0", as bash's does -- tsh appended to the key "" (bash's appendop1.sub;
# finding 296).
declare -A foo=([one]=bar [two]=baz)
foo+=zero
declare -p foo
foo+=([four]=four)
declare -p foo
foo+=-more
echo "${foo[0]}"
