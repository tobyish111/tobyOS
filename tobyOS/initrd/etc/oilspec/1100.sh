# bash doesn't respect these, but other shells do.  Gah!  I think bash
# behavior makes more sense.  It only escapes IFS.
echo '\a \b \c \d \e \f \g \h \x65 \145 \i' > $TMP/read-c.txt
read line < $TMP/read-c.txt
echo $line
