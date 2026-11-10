# An alias's text ends a token where the name did, so an operator written
# right after the name is still an operator: one='echo 1' makes `one>&2`
# echo 1 >&2 -- tsh joined the text and made `1>&2` a redirection (finding
# 471). A text ending in a backslash quotes the byte after it.
exec 2>&1
shopt -s expand_aliases
alias one='echo 1' nine='echo 9'
one>&2
one>/dev/null; echo "rc=$?"
nine<&-
alias pz='printf "%s\n" 2'
{ pz>&2; } 2>&1
alias bs='printf "[%s]\n" \'
bs|cat
