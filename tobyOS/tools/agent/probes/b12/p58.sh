x=${ for i in a b; do echo -$i-; done; }
echo "[$x] $?"
y=${|for i in a b; do REPLY+="-$i-"; done; }
echo "[$y] $?"
