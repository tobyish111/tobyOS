# exec {v}>&- with v unset is "v: ambiguous redirect", status 1, set -u or
# not; with a value that is not a number it closes nothing, quietly,
# status 0 (finding 452).
exec 2>&1
unset w; exec {w}>&-; echo "rc=$?"
w=abc; exec {w}>&-; echo "rc=$?"
"$BASH" -c 'unset v; set -u; exec {v}>&-; echo after' 2>&1 | sed 's/^.*line 1: //'
