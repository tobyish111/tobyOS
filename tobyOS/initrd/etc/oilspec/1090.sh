echo 'a b' > $TMP/read-few.txt

c='some value'
read a b c < $TMP/read-few.txt
echo "'$a' '$b' '$c'"

case $SH in dash) exit ;; esac # dash does not implement -n

c='some value'
read -n 3 a b c < $TMP/read-few.txt
echo "'$a' '$b' '$c'"
