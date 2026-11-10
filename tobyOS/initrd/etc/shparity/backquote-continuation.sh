# Inside backquotes a backslash-newline is a line continuation, whatever
# quotes the command in them has: bash's `echo 'foo\<newline>bar'` there is
# foobar, where tsh kept the pair (bash's quote.tests; finding 291).
echo `echo 'foo\
bar'`
echo "`echo 'foo\
bar'`"
echo `echo foo\
bar`
echo $(echo 'foo\
bar')
a=`echo 'a b c' | sed 's/ /\\
/g'`
echo "$a"
# ...and no quote inside backquotes is the reader's: "(\\")" there was a
# string left open, and the next two lines were glued on
echo `echo "(\\")"`
: `: "\\""`
echo `echo "(\")"`
echo `echo '\$' bab`
echo `echo '${'`
echo after-quotes $LINENO
