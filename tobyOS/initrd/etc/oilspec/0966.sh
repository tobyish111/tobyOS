command -V nonexistent 2>err.txt
echo status=$?
fgrep -o 'nonexistent: not found' err.txt || true
