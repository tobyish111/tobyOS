shopt -p nullglob invalid failglob
echo status=$?
# same thing as -p, slightly different format in bash
shopt nullglob invalid failglob > $TMP/out.txt
status=$?
sed --regexp-extended 's/\s+/ /' $TMP/out.txt  # make it easier to assert
echo status=$status
