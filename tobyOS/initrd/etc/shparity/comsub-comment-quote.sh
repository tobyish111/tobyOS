# A comment inside a $( ) or <( ) that runs over lines is not shell text:
# an apostrophe in it opens nothing (batch 37: logs/b37/quotemix.sh). Read
# as words, it opened a string the reader hunted for to the end of the
# script ("unexpected end of file").
x=$(echo a # it's a comment
); echo "$x"
read -r l < <(echo b # it's here
); echo "$l"
y=$(
  # don't stop here
  echo c
  echo d # and "here"
); echo "$y"
z=$(echo e#f); echo "$z"
n=$(v=abc; echo ${#v}); echo "$n"
echo "$(echo g # it's
)"
: $(: \ # not a comment)
echo "after escaped blank"
: $(: \;# not a comment )
echo "after escaped semicolon"
v=$(echo a\ #b); echo "[$v]"
echo end
