# FUNCNAME, BASH_LINENO and BASH_SOURCE describe the call stack: one entry
# per active function, innermost first, `main` at the bottom. tsh kept
# FUNCNAME to the current function and had no BASH_LINENO.
exec 2>&1
f() { echo "f: ${FUNCNAME[*]} | ${BASH_LINENO[*]} | ${#BASH_SOURCE[@]}"; g; }
g() { echo "g: ${FUNCNAME[*]} | ${BASH_LINENO[*]}"; h; }
h() { echo "h: ${FUNCNAME[0]} ${FUNCNAME[1]} ${#FUNCNAME[@]}"; }
f
echo "top: [${FUNCNAME[*]}] [${BASH_LINENO[*]}]"
