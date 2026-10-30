# A function's local reference that comes back to its own name -- or a
# chain of locals that comes back to where it began -- stands for the
# GLOBAL variable of that name, past a caller's local of it, with bash's
# "warning: NAME: circular name reference" at each use (finding 355). tsh
# wrote the local reference's own value, or failed the write.
exec 2>&1
f() { typeset -n v=$1; v=inside; }
v=global; f v; echo "[$v]"
g() { local v=mid; f v; echo "g [$v]"; }; v=top; g; echo "top [$v]"
h() { typeset -n w=w; w+=X; echo "in [$w]"; }; w=a; h; echo "out [$w]"
k() { local -n a=b; local -n b=a; a=1; echo "[$a] [$b]"; }; a=ga; b=gb; k; echo "[$a] [$b]"
m() { typeset -n q=q; read q <<< rd; echo "[$q]"; printf -v q %s pf; echo "[$q]"; }; q=0; m; echo "[$q]"
p() { typeset -n e=e; export e; declare -p e; }; e=1; p; declare -p e
