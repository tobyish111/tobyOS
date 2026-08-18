# zero
[ -0 -eq 0 ]
echo zero=$?

# octal numbers can be negative
[ -0123 -eq -83 ]
echo octal=$?

# hex doesn't have negative numbers?
[ -0xff -eq -255 ]
echo hex=$?

# base N doesn't either
[ -64#a -eq -10 ]
echo baseN=$?
