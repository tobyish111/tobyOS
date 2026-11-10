# GETCONF, LOGNAME AND PATHCHK ARE NOT BASH BUILTINS, and /bin/tsh does
# not carry them as builtins either: `type` names the file bash finds, or
# none, and `builtin` refuses them (finding 18; logs/b17 a2).
exec 2>&1
N() { sed "s#is /[^ ]*#is FILE#; s/^[a-z]*: /SH: /; s/^[^:]*: line [0-9]*: //"; }
type getconf logname pathchk 2>&1 | N
type -t logname pathchk; echo "t=$?"
builtin logname 2>&1 | N; echo "b=${PIPESTATUS[0]}"
enable -a | grep -c -e "enable getconf\$" -e "enable logname\$" -e "enable pathchk\$"
compgen -b | grep -c -x -e getconf -e logname -e pathchk
echo end
