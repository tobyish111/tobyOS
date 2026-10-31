# help with its stdout closed says nothing and is status 0 -- bash's help
# does not check its writes; echo, pwd and printf still report a write
# error (finding 456).
exec 2>&1
help >&- ; echo "rc=$?"; echo x >&- ; echo "rc=$?"; pwd >&- ; echo "rc=$?"; printf 'y\n' >&-; echo "rc=$?"
