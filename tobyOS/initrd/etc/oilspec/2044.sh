# dash and mksh allow this, which is a BUG.
# POSIX says: "Enclosing characters in single-quotes ( '' ) shall preserve the
# literal value of each character within the single-quotes. A single-quote
# cannot occur within single-quotes"
echo 'a\tb'

# See if it supports ANSI C escapes.  Bash supports this, but dash does NOT.  I
# guess dash you would do IFS=$(printf '\n\t')
