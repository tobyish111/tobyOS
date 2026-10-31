# An alias can open a quote that the rest of its line closes: with
# say="echo 'open:", the line `say rest'` is echo 'open: rest' -- tsh read
# on for the quote to the end of the script (finding 469).
exec 2>&1
shopt -s expand_aliases
alias say="echo 'open:"
say rest'
alias dsay='echo "dq:'
dsay more"
alias two="echo 'x"
two y' 'z'
echo after
