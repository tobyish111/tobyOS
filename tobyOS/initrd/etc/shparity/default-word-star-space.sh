# The word of ${A-W} or ${A+W} where no split follows -- an assignment's
# value -- joins $* (and its slices, and ${!a[*]}) with a space under a
# null IFS; ${a[*]} there runs its elements together. tsh ran them all
# together (finding 558).
exec 2>&1
set -- a 'b c' d; arr=(a 'b c' d)
IFS=
y=${A-$*}; echo "[$y]"
y=${A:-${*:1}}; echo "[$y]"
v=v; y=${v+$*}; echo "[$y]"
y=${A-${!arr[*]}}; echo "[$y]"
y=${A-${arr[*]:0}}; echo "[$y]"
y=${A-${arr[*]}}; echo "[$y]"
y=x${A-$*}y; echo "[$y]"
y=$*; echo "[$y]"
