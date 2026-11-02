# In posix mode a function may not take a special builtin's name: the
# definition is "`exec': is a special builtin" and a non-interactive shell
# ends, status 2 -- tsh said so and went on (finding 588).
for n in exec eval set; do
    out=$("$BASH" -o posix -c "$n() { echo fn; }; echo went on" 2>&1); st=$?
    echo "[$n] ${out#*line 1: } exit=$st"
done
out=$("$BASH" -c 'exec() { echo fn; }; exec; echo went on' 2>&1); st=$?
echo "[not posix] $out exit=$st"
