# Braces that are neither a list nor a range are text, and a list inside
# them still expands: `a-{b{d,e}}-c` is a-{bd}-c a-{be}-c -- tsh left the
# inner list alone (finding 510).
exec 2>&1
echo a-{b{d,e}}-c
echo {a{b,c}}
echo x{a{b,c}d}y
echo {{a,b}}
echo {x}_{a,b}
