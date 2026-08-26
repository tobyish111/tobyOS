printf '[%s]\n' '\044'  # escapes not evaluated
printf '[%b]\n' '\044'  # YES, escapes evaluated
echo

printf '[%s]\n' '\x7e'  # escapes not evaluated
printf '[%b]\n' '\x7e'  # YES, escapes evaluated
echo

# not a valid escape
printf '[%s]\n' '\A'
printf '[%b]\n' '\A'
