# In double quotes a $'...' in the value word of a ${ } is its decoded
# text, and bash reads the ${ } again with that text in it: its expansions
# and backslashes are the word's own, a decoded } closes it, and a lone
# quote is "bad substitution: no closing `}'". With extquote off, or in
# posix mode, the $'...' stays as written. tsh decoded it as data
# (finding 563).
exec 2>&1
HOME=/H
printf '<%s>\n' "${zz-$'c\x24HOME'}" "${zz-$'a\\\\b'}" "${zz-$'q\x7dr'}"
printf '<%s>\n' "${zz-$'it\'s'}"; echo "same line"
echo "rc=$?"
shopt -u extquote
printf '<%s>\n' "${zz-$'a\tb'}"
printf '<%s>\n' "${zz-$'it\'s'}"
shopt -s extquote
set -o posix
printf '<%s>\n' "${zz-$'it\'s'}"
