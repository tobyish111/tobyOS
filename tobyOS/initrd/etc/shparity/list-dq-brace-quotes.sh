# Inside a double-quoted list element, a ${...} carries its own quotes:
# `a2=("${a[@]/#/"-i '"}")` is a list of two, where tsh ended the element at
# the inner `"` and the lone `'` ran to the end of the line, "unterminated
# ${...}" (finding 397).
exec 2>&1
a=(x y); a2=("${a[@]/#/"-i '"}"); declare -p a2
set -- p q; b2=("${@/#/"-i '"}" "$(echo ")")"); declare -p b2
