echo $'\1 \11 \11 \111' | od -A n -c | sed 's/ \+/ /g'
