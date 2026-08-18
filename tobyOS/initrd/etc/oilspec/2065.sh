echo FOO > file2

# This only happens in command subs, which is weird
< file2 | tr A-Z a-z
( < file2 )
echo end
