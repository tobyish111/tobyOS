# POSIX 2.2.1 -- escape character (backslash).
# A backslash preserves the literal value of the next character, with the one
# exception of <newline>, where the pair is a line continuation and both
# characters are removed.
echo a\ b
echo a\\b
echo \$notavar
echo \"quoted\"
echo \'squoted\'
echo \#nothash
echo \*nostar
echo \~notilde
echo a\;b
echo one\
two
x=val
echo \$x is not $x
echo 'a'\''b'
echo "d"\""e"
echo end
