# fc's `-e -` is -s, and `--` ends its options -- tsh ran a command
# called `-`, and said "--: invalid option" (finding 551).
exec 2>&1
unset HISTFILE
set -o history
echo aa
fc -e - a=b
fc -e - echo
fc -l -- -2
fc -s -- 1
