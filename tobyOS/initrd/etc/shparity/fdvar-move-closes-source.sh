# A move into a {var} descriptor closes the descriptor moved, not the new
# one: after `exec {c}>&9-` a write to 9 is "9: Bad file descriptor" and $c
# works (finding 453).
exec 2>&1
exec 9>/dev/null; exec {c}>&9-; echo x >&9; echo "rc=$?"; echo y >&$c; echo "rc=$?"
exec {c}>&-
