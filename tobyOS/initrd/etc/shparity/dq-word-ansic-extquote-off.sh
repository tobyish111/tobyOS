# With extquote off, `$'...'` in a ${x-WORD} word inside double quotes is
# the bytes written; a pattern operator's word still decodes it -- tsh
# decoded both (finding 477). With extquote on the word decodes.
exec 2>&1
x=notOK
printf '<%s>\n' "${zz-$'a\tb'}" "${zz:+$'n'}" "${x:+$'p q'}"
shopt -u extquote
printf '<%s>\n' "${zz-$'a\tb'}" "${zz:-$'c d'}" "${x:+$'p q'}"
printf '<%s>\n' "${x#$'not'}" "${x%%$'K'}" "${x/$'O'/0}"
printf '<%s>\n' ${zz-$'e\tf'}
