# They both give a syntax error.  This is lame.
[[ '^(a b)$' == ^(a\ b)$ ]] && echo true
