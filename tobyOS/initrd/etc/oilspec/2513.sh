# LooksLikeGlob('[]') is true
# I guess dash, mksh, and zsh treat unquoted [ as an invalid glob?
var='[]foo[]'
echo ${var#[]}
echo ${var#"[]"}
echo "${var#[]}"
echo "${var#"[]"}"
