umask 0124
umask a=X
echo ret0 = $?
umask | tail -c 4

umask 0246
umask a=X
echo ret1 = $?
umask | tail -c 4

umask 0246
umask a-X
echo ret2 = $?
umask | tail -c 4
