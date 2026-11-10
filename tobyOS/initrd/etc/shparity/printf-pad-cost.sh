# printf's padding goes out in pieces. A byte at a time, each was a write
# of its own into a $( )'s spill file: 3 MB of it took 25 s in QEMU, where
# bash takes under 2 -- ten seconds is room no machine needs (finding 617).
exec 2>&1
timeout 10 "$BASH" -c 'x=$(printf "%*s" 3000000 ""); echo "${#x}"'; echo "rc=$?"
timeout 10 "$BASH" -c 'x=$(printf "%-*s|" 2000000 a); echo "${#x}"'; echo "rc=$?"
