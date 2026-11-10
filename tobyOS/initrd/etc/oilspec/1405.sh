# Shells don't agree here, some of them give you form feeds!
# There are two levels of processing I don't understand.

#echo BUG
#exit

echo `echo \\\"foo\\\"` -
echo `echo \\\\"foo\\\\"` -
echo `echo \\\\\"foo\\\\\"` -
