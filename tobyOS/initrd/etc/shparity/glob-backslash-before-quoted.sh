# A backslash from a variable ahead of a written `\*` is a backslash, and
# the quoted `*` a star: `$var\*b*` with var='a\' takes the file a\*b, not
# a*b (finding 463).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
var='a\'; touch 'a*b' 'a\*b'; printf '%s\n' $var\*b*
cd /; rm -rf "$d"
