declare -A A=(["x"]=y)
echo x=${!A[@]@a}
echo invalid=${!A@a}

# OSH prints 'a' for indexed array because the AssocArray with ! turns into
# it.  Disallowing it would be the other reasonable behavior.


# Bash succeeds with ${!A@a}, which references the variable named as $A (i.e.,
# '').  This must be a Bash bug since the behavior is inconsistent with the
# fact that ${!undef@a} and ${!empty@a} fail.
