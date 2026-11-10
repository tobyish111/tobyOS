a[1]=x
echo status=$?
argv.py "${a[@]}"

a[0+2]=y
#a[2|3]=y  # zsh doesn't allow this
argv.py "${a[@]}"

# += does appending
a[0+2]+=z
argv.py "${a[@]}"
