# An environment string whose name is no shell name -- `v[0]=help`, `a-b=c`
# -- is no variable, and passes through to the shell's children as it came
# (finding 387). tsh took `v[0]=help` for an element assignment, and the
# string was gone.
exec 2>&1
env 'v[0]=help' 'a-b=c' ok=1 "$BASH" -c 'printenv "v[0]"; printenv "a-b"; echo "[${v[0]-unset}] [$ok]"; ( printenv "a-b" )'
