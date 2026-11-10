case $SH in bash*|zsh) exit ;; esac

shopt --set parse_at

cat <(echo a; exit 2) <(echo b; exit 3)
echo status=$? ps @_process_sub_status

echo __
shopt -s process_sub_fail

cat <(echo a; exit 2) <(echo b; exit 3)
echo status=$? ps @_process_sub_status

# Now exit because of it
set -o errexit

cat <(echo a; exit 2) <(echo b; exit 3)
echo status=$? ps @_process_sub_status
