alias foo=bar
alias spam=eggs

alias | egrep 'foo|spam' | wc -l

unalias -a

alias
echo status=$?
