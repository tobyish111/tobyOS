f() { echo foo; echo bar; }

# Bash prints warnings: -C option may not work as you expect
#                       -F option may not work as you expect
#
# https://unix.stackexchange.com/questions/117987/compgen-warning-c-option-not-working-as-i-expected
#
# compexport fixes this problem, because it invokves ShellFuncAction, whcih
# sets COMP_ARGV, COMP_WORDS, etc.
#
# Should we print a warning?

compgen -C f b
echo compgen=$?

complete -C f b
echo complete=$?
