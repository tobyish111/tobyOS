# This is a bug fix; it used to cause problems with unescaping.
one='\'
two='\\'
echo $one $two
echo "$one" "$two"
