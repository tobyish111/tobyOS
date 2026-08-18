ref='a[@]'
a=('' '' '')

echo "==== check ===="

argv.py "${!ref:-set}"
argv.py "${a[@]:-set}"

echo "==== assign ===="

argv.py "${!ref:=assign}"
argv.py "${!ref}"
a=('' '' '') # revert the state in case it is modified

argv.py "${a[@]:=assign}"
argv.py "${a[@]}"
