# GAP 9: a redirection attached to a whole compound command.
# `while read l; do ...; done < file` is how every script reads a file line by
# line. The loop parsers stopped at `done` and ignored what followed, so the
# redirection was dropped and the body read from the terminal instead.
printf 'l1\nl2\nl3\n' > in.txt

while read line; do echo "w:$line"; done < in.txt

n=0
while read x; do n=$((n + 1)); done < in.txt
echo "count=$n"

# The loop's own output can be redirected too.
while read y; do echo "cap:$y"; done < in.txt > out.txt
while read z; do echo "back:$z"; done < out.txt

for f in in.txt out.txt; do echo "for:$f"; done

printf 'u1\nu2\n' > u.txt
k=0
until [ $k -ge 1 ]; do
    k=1
    while read w; do echo "nested:$w"; done < u.txt
done
echo redircompound-done
