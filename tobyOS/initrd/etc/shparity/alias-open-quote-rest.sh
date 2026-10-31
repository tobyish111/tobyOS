# After an alias that leaves a quote open, the text up to the quote's
# close is inside it, where no word is a command word -- not even after a
# replacement ending in a blank: with q="echo 'in: " and e0='', `q e0'` is
# echo 'in:  e0' -- tsh replaced the e0 (finding 470).
exec 2>&1
shopt -s expand_aliases
alias e0=''
alias q="echo 'in: "
eval "q e0'"
eval "q e0 e0'"
alias dq='echo "dq: '
eval 'dq e0 x"'
q e0'
echo after
