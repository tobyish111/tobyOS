# bash prints binary data!
case $SH in bash*|mksh) exit ;; esac

unquoted='foo'
sq='foo bar'
bash1=$'\x1f'  # ASCII control char
bash2=$'\xfe\xff'  # Invalid UTF-8

s1=$unquoted
s2=$sq
s3=$bash1
s4=$bash2

declare -a a=("$unquoted" "$sq" "$bash1" "$bash2")
declare -A A=(["$unquoted"]="$sq" ["$bash1"]="$bash2")

#echo lengths ${#s1} ${#s2} ${#s3} ${#s4} ${#a[@]} ${#A[@]}

declare -p s1 s2 s3 s4 a A | tee tmp.bash

echo ---

bash -c 'source tmp.bash; echo "$s1 $s2"; echo -n "$s3" "$s4" | od -A n -t x1'
echo bash=$?
