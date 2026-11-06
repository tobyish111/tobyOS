# ${*@A} unquoted under a null IFS is the "@" form's words: `set`, `--`,
# then each value quoted. tsh gave the whole statement as one word
# (finding 611).
exec 2>&1
o() { printf '<%s>' "$@"; echo " $#"; }
set -- a 'b c'; IFS=
o ${*@A}
o "${*@A}"
o ${@@A}
IFS=' '; o ${*@A}
