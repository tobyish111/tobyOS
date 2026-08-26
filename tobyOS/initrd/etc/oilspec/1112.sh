case $SH in dash|mksh) exit ;; esac

# file descriptor
read -s -u 3 3<<EOF
hi
EOF
echo reply=$REPLY
