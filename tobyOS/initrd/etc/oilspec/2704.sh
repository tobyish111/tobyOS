# Treated differently than the default IFS.  What is the rule here?
IFS='_'
char='_'
space=' '
empty=''
argv.py $char
argv.py $space
argv.py $empty
