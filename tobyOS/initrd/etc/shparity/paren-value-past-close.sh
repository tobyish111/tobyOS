# A value that goes on past its `)` is a string, not a list: `typeset -i a;
# a=(4*3)/2` is 6, and over an -i array its element 0 (finding 393). tsh
# took the list (4*3) and dropped the rest.
exec 2>&1
typeset -i a; a=(4*3)/2; echo "$a"; declare -p a
typeset -i b; b=(5+3); b=(4*3)/2; echo "$b"; declare -p b
