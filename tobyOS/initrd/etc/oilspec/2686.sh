#set -o noglob
HOME=/home/bob
str=s
array=(a1 a2)
argv.py bare 'sq' ~ $str "-${str}-" "${array[@]}" $((1+2)) $(echo c) `echo c`
