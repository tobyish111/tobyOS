# An expansion error inside an array literal discards the rest of the line,
# as bash's does; tsh carried on.
# (batch 12; probe p5 in C:\t\b12)
exec 2>&1
f() { a=( 1 ); a+=( $((1/0)) ); echo "after a=${a[*]}"; }
f; echo "status=$?"
g() { b=$((1/0)); echo "after b"; }
g; echo "status=$?"
h() { c=( x $((2/0)) y ); echo "after c=${c[*]}"; }
h; echo "status=$?"
echo end
