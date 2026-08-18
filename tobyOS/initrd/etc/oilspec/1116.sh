# hm DISABLED if we're not going to the terminal
# so we're only testing that it accepts the flag here

case $SH in dash|mksh|zsh) exit ;; esac

echo hi | { read -p 'P'; echo $REPLY; }
echo hi | { read -p 'P' -n 1; echo $REPLY; }
