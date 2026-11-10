typeset -A a
a=(aa b foo bar a+1 c)
a[X]=XX
argv.py "${a[@]}"
# What order is this?
