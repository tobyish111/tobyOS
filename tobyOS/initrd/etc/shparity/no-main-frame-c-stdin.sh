# Under -c and from standard input there is no script frame: no "main" at
# the bottom of FUNCNAME and no 0 at the bottom of BASH_LINENO -- tsh put
# both under every stack (finding 527).
exec 2>&1
"$BASH" -c 'f() { echo "[${FUNCNAME[*]}] [${BASH_LINENO[*]}]"; }; f'
printf '%s\n' 'f() { echo "[${FUNCNAME[*]}] [${BASH_LINENO[*]}]"; }' 'g() { f; }' g | "$BASH"
f() { echo "[${FUNCNAME[*]}] [${BASH_LINENO[*]}]"; }
f
