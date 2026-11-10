# bash gives empty string because it's like a[0]
# mksh gives the name of the variable with !.  Very weird.

a=(1 '2 3')
argv.py "${!a}"
