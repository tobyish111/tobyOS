# There is an obsolete comment in bash_completion that claims the opposite.
cd $TMP
touch 'foo bar'
touch "foo'bar"
compgen -f "foo b"
compgen -f "foo'"
