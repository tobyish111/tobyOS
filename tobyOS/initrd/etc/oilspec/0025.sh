shopt -s expand_aliases  # bash requires this
alias FOR1='for '
alias FOR2='FOR1 '
alias eye1='i '
alias eye2='eye1 '
alias IN='in '
alias onetwo='$one "2" '  # NOTE: this does NOT work in any shell except bash.
one=1
FOR2 eye2 IN onetwo 3; do echo $i; done
