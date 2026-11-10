# POSIX 2.2.2 -- single quotes preserve the literal value of EVERY character.
# There is no escape inside them at all, not even for a backslash.
x=expanded
echo 'plain'
echo '$x'
echo '`echo hi`'
echo '$(echo hi)'
echo '$((1+1))'
echo 'a\b'
echo 'a\\b'
echo 'tab	kept'
echo 'star * question ? bracket [a]'
echo 'semi;colon and |pipe| and &amp'
echo 'double " inside'
echo '  leading and trailing  '
echo 'multi
line'
echo ''
count() { echo "n=$#"; }
count 'a b c'
count '' ''
echo end
