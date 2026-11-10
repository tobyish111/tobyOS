# this confuses code and data
typeset -n ref='a[@]'
a=('A B' C)
argv.py ref "$ref"  # READ through ref works
ref=(X Y Z)    # WRITE through doesn't work
echo status=$?
argv.py 'ref[@]' "${ref[@]}"
argv.py ref "$ref"  # JOINING mangles the array?
argv.py 'a[@]' "${a[@]}"
