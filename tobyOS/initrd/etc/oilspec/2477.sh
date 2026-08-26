# out of range begin doesn't raise error in bash, but in mksh it skips the
# whole thing!
foo=abcdefg
echo _${foo:100:3}
echo $?
