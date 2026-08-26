file=$TMP/command-sub-dbracket
#rm -f $file
echo "123 `[[ $(echo \\" > $file) ]]` 456";
cat $file
