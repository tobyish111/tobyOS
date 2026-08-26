shopt -s expand_aliases
alias zero='echo 0'
a[$(zero)]=ZERO
a[1]=ONE
argv.py "${a[@]}"
