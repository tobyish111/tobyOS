case $SH in dash|ash|zsh) exit ;; esac

echo abcxyz | { read -n 3; echo reply=$REPLY; }

# zsh appears to hang with -k
