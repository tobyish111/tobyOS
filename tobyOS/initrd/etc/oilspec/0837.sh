echo -en 'abcd\x6' | od -A n -c | sed 's/ \+/ /g'
