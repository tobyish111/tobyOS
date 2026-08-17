# Redirection, read back through the `read` builtin so the case stays inside
# the shell and measures the SHELL rather than whichever `cat` happens to be
# installed. Runs in a scratch directory the harness wipes before each shell.
echo one > f1
echo two >> f1
while read line; do echo "f1:$line"; done < f1

echo three > f2
while read l; do echo "f2:$l"; done < f2

{ echo a; echo b; } > f3
while read l; do echo "brace:$l"; done < f3

echo overwritten > f1
while read l; do echo "again:$l"; done < f1

> empty.txt
n=0
while read l; do n=$((n + 1)); done < empty.txt
echo "emptylines=$n"
