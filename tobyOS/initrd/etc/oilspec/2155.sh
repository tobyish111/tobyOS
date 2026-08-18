# zsh and osh are stricter than bash.  bash treats [[ like a command.

[[ a =~ $(( 1 / 0 )) ]]
echo status=$?
