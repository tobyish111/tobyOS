set -o nounset
(( undef1++ ))
(( ++undef2 ))
echo "[$undef1][$undef2]"
