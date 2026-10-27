# An empty operand names no file: stat("") is ENOENT, so every file test
# is false (bash's test.tests). Canonicalized, "" was the current
# directory, and [ -d "" ], -e, -r, -w, -x, -s, -O and -G were true.
for o in -e -f -d -r -w -x -s -L -h -b -c -p -S -O -G -N -u -g -k; do
    [ $o "" ]; printf '%s=%s ' "$o" $?
done
echo
test -d ""; echo "test=$?"
[[ -d "" ]]; echo "dbr-d=$?"
[[ -e "" ]]; echo "dbr-e=$?"
dir=; if [ -d "$dir" ]; then echo "dir taken"; else echo "dir not taken"; fi
