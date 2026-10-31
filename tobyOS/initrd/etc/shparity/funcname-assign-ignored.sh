# An assignment to FUNCNAME has no effect, in a function or out of one,
# until it is unset -- tsh set it (finding 503).
exec 2>&1
FUNCNAME=7
echo "[$FUNCNAME]"
f() { FUNCNAME=8; echo "in [$FUNCNAME] [${FUNCNAME[1]}]"; }; f
echo "[${FUNCNAME[*]}]"
unset FUNCNAME
FUNCNAME=9
echo "[$FUNCNAME]"
