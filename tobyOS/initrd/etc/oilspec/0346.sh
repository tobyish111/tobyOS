x=1
y=2
a[$x$y]=foo

# not allowed by OSH parsing
#echo ${a[$x$y]}

echo ${a[12]}
echo ${#a[@]}
