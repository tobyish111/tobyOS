# POSIX alias: in a SCRIPT bash records aliases without expanding them, and
# `alias NAME` prints the definition in reinput form. unalias removes; both
# report unknown names with status 1 (messages on stderr, not compared).
alias walk_a='echo hi'
alias walk_a
echo "1=$?"
alias walk_b='echo two words'
alias | grep walk_b
alias walk_nosuch
echo "3=$?"
unalias walk_a
alias walk_a
echo "4=$?"
unalias walk_nosuch
echo "5=$?"
alias walk_c='echo c'
unalias -a
alias | grep -c walk_c
echo end
