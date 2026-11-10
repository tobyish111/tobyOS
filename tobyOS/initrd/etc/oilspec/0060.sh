# bash - runtime error: cannot assign list to array number
# mksh - a[-1]+: is not an identifier
# osh - parse error -- could be better!
a=(1 '2 3')
a[-1]+=(4 5)
argv.py "${a[@]}"
