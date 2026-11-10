# export takes -a and -A as declare does: `export -a r='(7)'` is the
# exported array r=(7), `export -A s=([k]=v)` an associative one, and a
# bare `export -a` lists the exported indexed arrays -- tsh called them
# invalid options (finding 490).
exec 2>&1
f() { export -a r=(6); }; f; declare -p r
f() { export -a r='(7)'; }; f; declare -p r
export -A s=([k]=v); declare -p s
export -a; echo "rc=$?"
export -A; echo "rc=$?"
q=(1); readonly q
export -a q='(2)'; echo "rc=$?"
export -z x; echo "rc=$?"
