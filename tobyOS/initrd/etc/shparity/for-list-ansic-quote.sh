# A for list's words are skipped to its `do` with `$'...'` read as ANSI-C
# quoting, where `\'` is a quote inside it: `for x in $'a\'b' c` has two
# words -- tsh read the first as '...' closed at the `\'` and missed the
# `do` (finding 466, after 444).
exec 2>&1
for x in $'a\'b' c; do echo "[$x]"; done
for x in $'p\'q;r' s; do echo "[$x]"; done; echo after
for x in $'\\' $'\'' z; do printf '<%s>' "$x"; done; echo
