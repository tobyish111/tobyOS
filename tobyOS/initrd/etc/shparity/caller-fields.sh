# caller: "LINE FILE" with no operand, and for N "LINE NAME FILE" -- the
# name and file of the frame below N's; "0 NULL" at a script's top, and
# status 1 past the stack. tsh said "LINE main" and "LINE NAME main", NAME
# the frame's own (finding 531).
exec 2>&1
f() {
    caller; echo "rc=$?"
    caller 0; echo "rc=$?"
    caller 1; echo "rc=$?"
    caller 2; echo "rc=$?"
}
f
g() { f; }
g
caller; echo "rc=$?"
caller 0; echo "rc=$?"
