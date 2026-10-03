exec 2>&1
[[ ';' =~ ^\; ]]; echo "a $?"
[[ ';' =~ \; ]]
echo "b $?"
x='^\;'; [[ ';' =~ $x ]]; echo "c $?"
[[ 'a;' =~ a\; ]] && echo "d yes"
[[ 'x' =~ [[:alpha:]] ]]; echo "e $?"
[[ 'x' =~ ^[[:alpha:]]+$ ]]; echo "f $?"
[[ ' ' =~ ^[,]|^[[:space:]]+ ]]; echo "g $?"
