cd $TMP

# print specific options.  OSH does it in a different format.
shopt nullglob failglob > one.txt
wc -l one.txt
grep -o nullglob one.txt
grep -o failglob one.txt

# print all options
shopt | grep nullglob | wc -l
