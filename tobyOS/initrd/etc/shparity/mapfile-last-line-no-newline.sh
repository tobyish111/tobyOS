# mapfile without -t keeps each line's newline -- and a last line with
# none stays without one: tsh gave it a newline, in the array and in the
# callback's argument (finding 520).
exec 2>&1
f=${TMP:-/tmp}/mfln.$$
printf 'one\ntwo\nlast' > "$f"
mapfile A < "$f"
printf '<%s>' "${A[@]}"; echo
mapfile -C 'printf "[%s:%s]"' -c 1 B < "$f"; echo
printf 'x\ny\n' > "$f"
mapfile C < "$f"
printf '<%s>' "${C[@]}"; echo
printf 'a,b,c' > "$f"
mapfile -d , D < "$f"
printf '<%s>' "${D[@]}"; echo
rm -f "$f"
