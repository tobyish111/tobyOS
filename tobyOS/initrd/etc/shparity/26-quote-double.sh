# POSIX 2.2.3 -- double quotes. Only $ ` \ and " keep special meaning, and a
# backslash is only special when it precedes one of those (or newline).
# Everywhere else a backslash inside double quotes is a literal backslash --
# the rule most shells-in-progress get wrong.
x=val
echo "plain"
echo "$x"
echo "${x}"
echo "$(echo sub)"
echo "$((2+3))"
echo "\$x"
echo "\"quoted\""
echo "\\backslash"
echo "a\bc"
echo "a\*b"
echo "star * stays literal"
echo "question ? stays literal"
echo "semi;colon |pipe| stays"
echo "single ' inside"
echo "  spacing   preserved  "
echo "multi
line"
count() { echo "n=$#"; }
count "a b c"
count "$x $x"
echo end
