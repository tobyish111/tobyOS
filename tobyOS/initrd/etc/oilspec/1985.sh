shopt --set parse_at

cat <(seq 2; exit 2) <(seq 3; exit 3)

case $SH in bash*|zsh) exit ;; esac

echo status @_process_sub_status
echo done
