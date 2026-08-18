umask 0124
umask 'u-r,u-r'
echo status=$?
umask

# syntax error
umask 'u+r,,u-r'
if test $? -ne 0; then echo 'error'; fi
umask
