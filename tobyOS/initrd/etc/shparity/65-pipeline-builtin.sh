# A BUILTIN STAGE THAT IS NOT THE LAST ONE RUNS IN A CHILD.
#
# Running the stages in order, in one process, means stage 0 fills the pipe
# and then blocks with nobody to drain it -- the reader is the same process
# and has not started yet. `cat </dev/zero | true` hung the shell.
echo one two three | { read a b c; echo "A=$a B=$b C=$c"; }
printf 'x
y
z
' | wc -l
echo hello | cat
echo a; echo b | cat
seq_out() { echo p; echo q; }
seq_out | cat
v=outer
echo inner | { read v; echo "in=$v"; }
echo "after=$v"
echo done
