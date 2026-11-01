# An alias's text that leaves an arithmetic (( or $(( open is no token at
# its end: the text after the name goes on with the expression, so its `)`
# closes the `))` -- tsh's blank there made `) )` close a $( ( ) ) and ran
# the expression as a command (finding 555). Inside a $( ) the name's end is
# still a token's end.
exec 2>&1
shopt -s expand_aliases
alias m='echo $(( 4+3 )'
m)
m)x
alias a='(( x = 4+3 )'
a)
echo "x=$x"
alias f='for (( i=0; i<2; i++ )'
f) do echo "i=$i"; done
alias two='echo $(( 1+2 )) $(( 4+3 )'
two)
alias in='echo $(echo $(( 4+3 )'
in))
alias s='echo $(echo 1'
s>&2)
