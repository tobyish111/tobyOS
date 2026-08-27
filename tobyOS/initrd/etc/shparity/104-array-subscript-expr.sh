# Subscripts are ARITHMETIC CONTEXT: a bare name is a variable, expressions
# evaluate, and a negative index counts back from the end.
a=(zero one two three four)
i=2
echo "var=${a[i]} dollar=${a[$i]}"
echo "expr=${a[i+1]} mul=${a[i*2]}"
echo "neg1=${a[-1]} neg2=${a[-2]}"
echo "zero=${a[0]} last=${a[${#a[@]}-1]}"
b[i]=set-via-var
echo "assign-var=${b[2]}"
b[i+3]=five
echo "assign-expr=${b[5]} n=${#b[@]}"
echo end
