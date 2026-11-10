# In posix mode a special builtin whose redirection fails ends a
# non-interactive shell, status 1 -- `:`, `set`, `exec`, and one an eval
# runs -- but not through `command`, and not outside posix mode; tsh went
# on (finding 587).
for c in ': </nonexistent/x' 'set -- a </nonexistent/x' \
         'exec 5</nonexistent/x' 'eval ": </nonexistent/x"' \
         'command : </nonexistent/x' 'command exec 5</nonexistent/x'; do
    out=$("$BASH" -o posix -c "$c; echo \"went on \$?\"" 2>&1); st=$?
    echo "[$c] ${out#*line 1: } exit=$st"
done
out=$("$BASH" -c ': </nonexistent/x; echo "went on $?"' 2>&1); st=$?
echo "[not posix] ${out#*line 1: } exit=$st"
