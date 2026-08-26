cd $TMP
touch foo=a foo=b
foo=*
argv.py "$foo"
unset foo

export foo=*
argv.py "$foo"
unset foo
