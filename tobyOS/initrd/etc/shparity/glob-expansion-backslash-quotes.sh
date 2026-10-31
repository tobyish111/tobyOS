# A backslash an unquoted expansion gives quotes the byte after it, so a
# word whose only glob character it quotes is no pattern: $v with v='a\?'
# stays a\? even with a file a? there -- tsh globbed it (finding 459).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit; touch 'a?' aa abcdefg
var='a\?'; printf '%s\n' ${var}; var='a\a'; printf '%s\n' ${var}
set -- a \?; IFS=\\; v2=$*; unset IFS; printf '%s\n' ${v2}
pat='ab\cd*'; printf '<%s>\n' $pat; pat='\.'; printf '<%s>\n' $pat
cd /; rm -rf "$d"
