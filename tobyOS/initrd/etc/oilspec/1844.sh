set -- one two three

x=X

ref='1'
echo ref=$ref
typeset -n ref
echo ref=$ref

# BUG: This is really assigning '1', which is INVALID
# with strict_nameref that degrades!!!
ref2='$1'
echo ref2=$ref2
typeset -n ref2
echo ref2=$ref2

x=foo

ref3='x'
echo ref3=$ref3
typeset -n ref3
echo ref3=$ref3
