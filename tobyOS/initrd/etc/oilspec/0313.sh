# mksh ignores it
foo=bar
a=('1 2' foo '2 3')
argv.py "${!a[1]}"
