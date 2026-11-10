shopt -s expand_aliases
alias t1='echo one && echo two && echo 3 | wc -l;
echo four'
t1
