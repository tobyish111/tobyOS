a=('1 3' 5)
b=('1 3' 5)
c=('1' '3 5')
d=('1' '3 6')

# shells EXPAND a and b first
(( a == b ))
echo status=$?

(( a == c ))
echo status=$?

(( a == d ))
echo status=$?
