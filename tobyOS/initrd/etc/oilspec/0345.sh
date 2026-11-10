shopt -u strict_arith
a=(1 2 3)
(( x = a[a] ))
echo $x
