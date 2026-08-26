file='foo bar'

echo hi > $file
echo status=$?

cat "$file"
echo status=$?
