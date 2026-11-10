# oil 0.8.pre4 fails to restore fds after redirection failure. In the
# following case, the fd frame remains after the redirection failure
# "2> /" so that the effect of redirection ">/dev/null" remains after
# the completion of the command.
: >/dev/null 2> /
echo hello
# dash/mksh terminates the execution of script on the redirection.
