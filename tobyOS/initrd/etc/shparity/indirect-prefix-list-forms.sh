# ${!PREFIX*} and ${!PREFIX@} list names only where PREFIX starts as a name
# does and the * or @ ends the expansion: ${!1*}, ${!@*} and ${!_Q* } are
# bad substitutions, status 1 and the rest of the line dropped (finding
# 400).
exec 2>&1
_Qa=1 _Qb=2; echo "${!_Q*}" "${!_Q@}"
echo "${!_Q* }"; echo "not here"
echo "rc=$?"; set -- a b; echo ${!1*}; echo "not here"
echo "rc=$?"; echo ${!@*}
echo "rc=$?"
