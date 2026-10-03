# $(( )) that is really $( ( ... ) ), `${ cmd; }` as a bad substitution, and
# arithmetic across lines.
# (batch 12; probe p58 in C:\t\b12)
exec 2>&1
x=${ for i in a b; do echo -$i-; done; }
echo "[$x] $?"
y=${|for i in a b; do REPLY+="-$i-"; done; }
echo "[$y] $?"
