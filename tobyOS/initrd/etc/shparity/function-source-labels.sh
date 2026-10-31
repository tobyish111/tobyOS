# A function defined with no file says so in BASH_SOURCE and declare -F:
# "environment" when it is imported or defined under -c, "main" from
# standard input -- tsh had nothing, or "main" for every one (finding 529).
exec 2>&1
"$BASH" -c 'f() { echo "[${BASH_SOURCE[*]}]"; }; f; shopt -s extdebug; declare -F f'
printf '%s\n' 'f() { echo "[${BASH_SOURCE[*]}]"; }' f 'shopt -s extdebug' 'declare -F f' | "$BASH"
g() { echo "[${BASH_SOURCE[*]##*/}]"; }
export -f g
"$BASH" -c 'g'
