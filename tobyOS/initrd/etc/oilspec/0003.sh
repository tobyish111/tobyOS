shopt -s expand_aliases
alias e=echo; e one  # this is not alias-expanded because we parse lines at once
e two; e three
