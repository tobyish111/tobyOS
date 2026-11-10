shopt -s extglob
rm bad_*

# They actually write this literal file!  This is what EvalWordToString() does,
# as opposed to _EvalWordToParts.
echo foo > bad_@(*.cc|*.h)
echo status=$?

echo bad_*

shopt -s failglob

# Note: ysh:ugprade doesn't allow extended globs
# shopt -s ysh:upgrade

echo foo > bad_@(*.cc|*.h)
echo status=$?
