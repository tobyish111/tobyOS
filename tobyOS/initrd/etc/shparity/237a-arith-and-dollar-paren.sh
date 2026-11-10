# $(( )) that is really $( ( ... ) ), `${ cmd; }` as a bad substitution, and
# arithmetic across lines.
# (batch 12; probe p57 in C:\t\b12)
exec 2>&1
echo "1 $(( (1+2) ))"
echo "2 $(( (1) + (2) ))"
echo "3 $((1) + (2))"
echo "4 $(( echo x ) )"
echo "5 $((echo y) )"
echo "6 $( (echo z) )"
