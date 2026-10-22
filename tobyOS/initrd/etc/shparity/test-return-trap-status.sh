# The RETURN trap's $? after `return N` (finding 30): bash runs the trap
# with $? as `return` found it -- the last command's status -- and the
# return value becomes $? only after the trap, in a sourced file and in a
# function alike. tsh gave the trap N.
d=${TMPDIR:-/tmp}/rts.$$
mkdir -p "$d"
printf 'return 3\n' > "$d/f1"
printf 'false; return 3\n' > "$d/f2"
printf 'true; return 3\n' > "$d/f3"
printf 'echo x\n' > "$d/f4"
printf 'false\nreturn\n' > "$d/f5"
printf '(exit 7); return\n' > "$d/f6"
printf 'return 3; echo no\n' > "$d/f7"
trap 'echo "trap=$?"' RETURN
false; . "$d/f1"; echo "after=$?"
true; . "$d/f1"; echo "after=$?"
true; . "$d/f2"; echo "after=$?"
false; . "$d/f3"; echo "after=$?"
false; . "$d/f4"; echo "after=$?"
true; . "$d/f5"; echo "after=$?"
true; . "$d/f6"; echo "after=$?"
false; . "$d/f7"; echo "after=$?"
false; . "$d/f2" a b; echo "after=$?"
set -T
f() { false; return 3; }
f; echo "after=$?"
g() { true; return; }
false; g; echo "after=$?"
h() { false; }
h; echo "after=$?"
k() { (exit 5); return 2; }
k; echo "after=$?"
set +T
trap - RETURN
rm -rf "$d"
