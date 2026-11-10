# arithmetic has octal conversion
echo $(( 083 ))
echo status=$?

echo $(( -083 ))
echo status=$?

echo

# Bracket does NOT have octal conversion!  That is annoying.
[ 083 -eq 83 ]
echo status=$?

[ -083 -eq -83 ]
echo status=$?
