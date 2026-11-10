# this confuses code and data
typeset -n ref='a[$(echo 2) + 1]'
a=(zero one two three)
echo ref=$ref
