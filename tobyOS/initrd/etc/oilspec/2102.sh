# Hm this seems like a failure of lookahead!  The second thing should look to a
# file-like thing.
# I think this is a posix issue.
# tag: posix-issue
echo one 1>&2
echo two 1 >&2
echo three 1>& 2
