# A $( ) in a redirection's word runs when that redirection is applied,
# after the command's earlier ones: what it writes to fd 2 goes where they
# have put fd 2 -- tsh ran it while reading the line, on its own stderr
# (finding 582).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
f() { echo f-says >&2; echo 1; }
echo one 2>/dev/null >&$(f); echo "rc=$?"
echo two >&$(f) 2>/dev/null; echo "rc=$?"
g() { echo g-says >&2; echo name; }
echo three 2>/dev/null >$(g).out; cat name.out
echo four >log 2>&1 2>$(g).f; cat log; ls name.f
echo five >log2 2>&1 >$(g).o; cat log2 name.o
cd /; rm -rf "$d"
