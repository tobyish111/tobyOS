# A letter range across the punctuation between Z and a makes words of
# it: a backquote alone is a backquote -- with text after it, as in
# -{Z..a}-, it opens a substitution that never closes -- and a backslash
# is an empty word. tsh called the lone backquote unclosed (finding 509).
exec 2>&1
printf '<%s>' {Z..a}; echo
printf '<%s>' {a..Y}; echo
echo {a..A}
echo -{Z..a}-; echo "rc=$?"
echo "after $?"
