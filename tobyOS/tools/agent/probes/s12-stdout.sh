# Results go to stdout, not stderr: what `getconf PATH` and `fc -l` print
# must be capturable by $( ), and `set -v`'s echo carries no prefix.
x=$(getconf PATH 2>/dev/null); echo "getconf PATH on stdout: $([ -n "$x" ] && echo yes || echo no)"
set -o history
echo one >/dev/null
x=$(fc -l 2>/dev/null); echo "fc -l on stdout: $([ -n "$x" ] && echo yes || echo no)"
set +o history
set -v
: verbose-line
set +v
echo done
