a=$(( 1 << 31 ))
echo $a

b=$(( a + a ))
echo $b

c=$(( b + a ))
echo $c

x=$(( 1 << 62 ))
y=$(( x - 1 ))
echo "max positive = $(( x + y ))"

#echo "overflow $(( x + x ))"


# mksh still uses int!
