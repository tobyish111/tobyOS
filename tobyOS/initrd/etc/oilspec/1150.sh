# NOTE: not testing arrays and associative arrays!
_space='[ ]'
_whitespace=$'[\t\r\n]'
_sq="'single quotes'"
_backslash_dq="\\ \""
_unicode=$'[\u03bc]'

# Save the variables
varfile=$TMP/vars-$(basename $SH).txt

set | grep '^_' > "$varfile"

# Unset variables
unset _space _whitespace _sq _backslash_dq _unicode
echo [ $_space $_whitespace $_sq $_backslash_dq $_unicode ]

# Restore them

. $varfile
echo "Code saved to $varfile" 1>&2  # for debugging

test "$_space" = '[ ]' && echo OK
test "$_whitespace" = $'[\t\r\n]' && echo OK
test "$_sq" = "'single quotes'" && echo OK
test "$_backslash_dq" = "\\ \"" && echo OK
test "$_unicode" = $'[\u03bc]' && echo OK
