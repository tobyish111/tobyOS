# A fatal signal the EXIT trap waits for, coming while the shell reads a
# $( )'s output, ends the shell there: the trap runs and the status is
# 143, and the command the substitution is part of never runs (finding
# 604). The TERM comes at once from a grandchild whose stdout is not the
# pipe, while the child fills the pipe, so the shell takes the signal at a
# read that returns data and the child is gone before the shell waits for
# it -- tsh printed "ran" there, every run. (A TERM sent later would need
# the stream to outlast it, which is the machine's speed, not the shell's.)
{ "$BASH" -c 'trap "echo trap \$BASH_SUBSHELL" EXIT; printf "ran\n%.0s" "$( { kill -TERM $$; } > /dev/null 2>&1 & printf "%*s" 1000000 "" )"; echo after'; } 2>/dev/null
echo "status $?"
