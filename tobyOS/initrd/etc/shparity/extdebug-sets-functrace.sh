# shopt -s extdebug turns functrace and errtrace on, and -u turns them
# off -- tsh left both as they were (finding 536).
exec 2>&1
t() { [[ -o functrace ]] && echo "T on" || echo "T off"; [[ -o errtrace ]] && echo "E on" || echo "E off"; }
t
shopt -s extdebug
t
f() { echo in; }
trap 'echo "D $LINENO"' DEBUG
f
trap - DEBUG
set -T
shopt -u extdebug
t
