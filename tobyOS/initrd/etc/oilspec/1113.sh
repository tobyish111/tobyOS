case $SH in dash|mksh) exit ;; esac

# file descriptor
read -u 3 -d 5 3<<EOF
123456789
EOF
echo reply=$REPLY
