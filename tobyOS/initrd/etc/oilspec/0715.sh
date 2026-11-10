# arithmetic has octal conversion
echo $(( 073 ))
echo $(( -073 ))

echo

# Bracket does NOT have octal conversion!  That is annoying.
[ 073 -eq 73 ]
echo status=$?

[ -073 -eq -73 ]
echo status=$?

echo

[ 0xff -eq 255 ]
echo hex=$?
[ 64#a -eq 10 ]
echo baseN=$?
