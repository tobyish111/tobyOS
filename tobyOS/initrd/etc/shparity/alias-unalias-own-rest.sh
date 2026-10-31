# An alias that eval runs and that removes itself in its own text leaves
# the rest of that text to be read after the command that ran eval: with
# z='unalias z<newline>n=2', `eval z; echo $n` prints the old n, and the
# next line sees 2 -- tsh ran the rest inside the eval (finding 474).
# Removing another alias leaves the text where it is.
exec 2>&1
shopt -s expand_aliases
n=1
alias z='unalias z
n=2'
eval z; echo "same line n=$n"
echo "next line n=$n"
alias z='unalias -a
echo second'
eval z; echo first
alias z='unalias z
echo tail'
{ eval z; echo in-group; }; echo after-group
alias other=true
alias z='unalias other
echo kept'
eval z; echo same
echo end
