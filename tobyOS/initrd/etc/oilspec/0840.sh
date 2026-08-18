echo -en 'abcd\u006' | od -A n -c | sed 's/ \+/ /g'
