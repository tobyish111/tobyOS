case $SH in ash|zsh) exit ;; esac

# file descriptor
read -u 3 -d b -N 4 3<<EOF
ababababa
EOF
echo reply=$REPLY
# test end on EOF
read -u 3 -d b -N 6 3<<EOF
ab
EOF
echo reply=$REPLY
