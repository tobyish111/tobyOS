a=('-x-' 'y-y' '-z-')

# This does the prefix stripping FIRST, and then it joins.
argv.py "${a[*]#-}"
