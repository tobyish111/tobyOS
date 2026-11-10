# `${#` and one byte: a length -- of $? or $- for those -- or, for an
# operator with nothing to work on, a bad substitution: ${#/} ${#%} ${#=}
# ${#+} ${#^} ${#,} -- tsh gave $# for each (finding 501). With a word
# after it the operator is $#'s.
exec 2>&1
set -- a b
for e in '${#?}' '${#/}' '${#%}' '${#=}' '${#+}' '${#^}' '${#,}' '${#/x}' '${#%x}' '${#=x}' '${#+x}' '${##}' '${#}'; do
  printf '%s -> ' "$e"; eval "echo \"[$e]\"" 2>&1 | sed 's/^.*line [0-9]*: //'
done
