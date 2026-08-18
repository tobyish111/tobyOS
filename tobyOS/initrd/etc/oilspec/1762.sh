case $SH in *osh) exit ;; esac

# SKIPPED: hangs with osh on Debian
# The second instance reads its stdin from the pipe, and fd 5 from a here doc.
read_from_fd.py 3 3<<EOF3 |
fd3
EOF3
read_from_fd.py 0 5 5<<EOF5
fd5
EOF5

echo ok
