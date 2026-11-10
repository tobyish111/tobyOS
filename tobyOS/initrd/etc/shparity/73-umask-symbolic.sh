# POSIX umask -S: the SYMBOLIC REPORT of the mask. Found by walking the
# standard: nothing in 2,776 third-party cases ever passed -S, and tsh
# rejected it as an invalid mode. Every probe runs in a subshell so the
# case's own mask never leaks into the next probe.
( umask 022; umask -S )
( umask 0;   umask -S )
( umask 077; umask -S )
( umask 137; umask -S )
( umask 022; umask -S u=rwx,g=rx,o=; echo "5=$?"; umask )
( umask 027; umask -S 022; echo "6=$?" )
( umask 022; umask -p )
( umask 022; umask -S -p )
( umask 022; umask -Sp )
( umask 022; umask -S -p 077; echo "10=$?"; umask )
( umask 022; umask -p 077; echo "11=$?"; umask )
( umask 027; umask -S 'u+r,,u-r'; echo "12=$?"; umask )
echo end
