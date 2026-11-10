umask 0124
umask -rwx
if test $? -ne 0; then echo 'error'; fi
umask | tail -c 4

umask 0124
umask -wx
if test $? -ne 0; then echo 'error'; fi
umask | tail -c 4

umask 0124
umask -=+
if test $? -ne 0; then echo 'error'; fi
umask | tail -c 4
