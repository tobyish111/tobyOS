# declare -a NAME[SUB]='(...)' -- a list a quote made -- is a list for the
# whole NAME, the subscript let go; without -a it is the element's value,
# and over a name that was not there bash warns the form is deprecated
# (finding 391). tsh gave the element the text every time.
exec 2>&1
declare -a e[10]=test; declare -p e; declare -a e[10]='(test)'; declare -p e
declare -a a[1]='(var)'; declare -p a
unset b; declare b[1]='(x y)'; declare -p b
declare -a w=(1 2); declare w[1]='(z)'; declare -p w
