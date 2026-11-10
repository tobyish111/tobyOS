cd $TMP
touch foo=a foo=b
typeset foo=*
argv.py "$foo"
unset foo
