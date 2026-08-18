alias ex=exit ll='ls -l'
alias | grep -E 'ex=|ll='  # need to grep because mksh/zsh have builtin aliases
echo status=$?
