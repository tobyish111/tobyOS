# Is there a simpler test case for this?
echo foo51 > $TMP/lessamp.txt

exec 6< $TMP/lessamp.txt
read line <&6

echo "[$line]"
