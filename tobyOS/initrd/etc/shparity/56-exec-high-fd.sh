# `exec N<file` AND `exec N>&1` FOR N ABOVE WHAT THE SHELL HAPPENED TO OPEN.
#
# Regression test for a kernel bug, not a shell one: fcntl's fall-through
# answered 0 for every command on every descriptor, INCLUDING closed ones.
# bash uses that answer to decide whether a redirection target needs saving,
# so it believed fd 6 was in use, tried to stash it with F_DUPFD, got the
# correct EBADF back, and reported "cannot duplicate fd". Every high-fd exec
# redirection failed -- the shape ./configure scripts are made of -- and the
# conformance oracle was wrong on three cases where tsh was right.
echo hello > f.txt
exec 6< f.txt
echo "A rc=$?"
read line <&6
echo "B rc=$? line=[$line]"
exec 6<&-
echo "C rc=$?"
exec 7>out7.txt
echo seven >&7
exec 7>&-
cat out7.txt
exec 3>&1
echo "D rc=$?"
exec 4>&1
echo "E rc=$?"
echo three >&3
echo four >&4
exec 3>&- 4>&-
echo done
