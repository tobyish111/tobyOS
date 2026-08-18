shopt -s strict_word_eval || true
echo slice
s=$(echo -e "\xFF")bcdef
echo -${s:1:3}-
