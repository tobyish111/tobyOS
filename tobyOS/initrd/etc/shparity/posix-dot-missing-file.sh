# In posix mode `.` of a file it cannot find ends a non-interactive shell,
# status 1 -- a name without a slash, not searched for in the working
# directory, is ".: NAME: file not found" -- unless `command` runs it; outside
# posix mode the shell goes on (finding 427).
exec 2>&1
"$BASH" -c 'command . notthere ; echo one' bash; echo "rc=$?"
"$BASH" -c '. notthere ; echo two' bash; echo "rc=$?"
"$BASH" -o posix -c 'command . notthere ; echo one' bash; echo "rc=$?"
"$BASH" -o posix -c '. notthere ; echo two' bash; echo "rc=$?"
"$BASH" -o posix -c '. /tmp/source-notthere ; echo three' bash; echo "rc=$?"
"$BASH" -o posix -c 'source ./notthere ; echo four' bash; echo "rc=$?"
