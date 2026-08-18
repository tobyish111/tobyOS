# echo -e syntax is echo -e \0377
echo -n $'\001' $'\377' | od -A n -c | sed 's/ \+/ /g'
