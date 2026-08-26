umask 0022

# osh and other shells treat truncate 0o1234567 as 0o0567
umask 1234567
echo status=$?

umask
