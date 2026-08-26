# POSIX fc: a non-interactive shell has no command history, and bash's
# `fc -l` on an empty history prints NOTHING and succeeds. The builtin has
# to exist and know that much; the interactive editing behaviour is not
# reachable from the gate.
fc -l
echo "1=$?"
echo end
