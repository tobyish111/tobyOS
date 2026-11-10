[[ { =~ "{" ]] && echo 'yes {'
[[ + =~ "+" ]] && echo 'yes +'
[[ * =~ "*" ]] && echo 'yes *'
[[ ? =~ "?" ]] && echo 'yes ?'
[[ ^ =~ "^" ]] && echo 'yes ^'
[[ $ =~ "$" ]] && echo 'yes $'
[[ '(' =~ '(' ]] && echo 'yes ('
[[ ')' =~ ')' ]] && echo 'yes )'
[[ '|' =~ '|' ]] && echo 'yes |'
[[ '\' =~ '\' ]] && echo 'yes \'
echo ---

[[ . =~ "." ]] && echo 'yes .'
[[ z =~ "." ]] || echo 'no .'
echo ---

# This rule is weird but all shells agree.  I would expect that the - gets
# escaped?  It's an operator?  but it behaves like a-z.
[[ a =~ ["a-z"] ]]; echo "a $?"
[[ - =~ ["a-z"] ]]; echo "- $?"
[[ b =~ ['a-z'] ]]; echo "b $?"
[[ z =~ ['a-z'] ]]; echo "z $?"

echo status=$?
