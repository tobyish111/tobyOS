# It is 256 % 256 which gets interpreted as a NUL byte.
echo -en '\04000' | od -A n -t x1 | sed 's/ \+/ /g'
