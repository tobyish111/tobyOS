# A `for` or `select` whose name is no name is "`WORD': not a valid
# identifier", WORD the whole word -- its quotes and its $( ) with it, the
# $( ) as bash keeps it (printed) -- and the script goes on, status 1. tsh
# cut the word at its first blank: `$(echo   a)' was `$(echo' (finding
# 646).
exec 2>&1
eval 'for $(echo   a) in 1; do :; done'; echo "for: $?"
eval 'for a"$(echo   b)" in 1; do :; done'; echo "quoted: $?"
eval 'for x$(echo  y;echo z) in 1; do :; done'; echo "a list: $?"
eval 'select $(echo   a) in 1; do :; done'; echo "select: $?"
eval 'for 1x in 1; do :; done'; echo "a digit: $?"
