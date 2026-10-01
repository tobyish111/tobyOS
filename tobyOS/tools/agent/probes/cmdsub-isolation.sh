# What a $( ) command substitution changes must not reach the shell that
# ran it -- it is a subshell (POSIX 2.6.3, 2.12). One line per kind of state.
cd /tmp; here=$PWD
x=$(y_cs=5); echo "variable: [${y_cs-unset}]"
x=$(cd /); echo "cwd: [$([ "$PWD" = "$here" ] && echo same || echo "$PWD")]"
x=$(f_cs() { echo f; }); type f_cs >/dev/null 2>&1 && echo "function: leaked" || echo "function: kept inside"
x=$(set -f); case $- in *f*) echo "option: leaked";; *) echo "option: kept inside";; esac
x=$(trap 'echo trapped' USR1); echo "trap: [$(trap -p USR1)]"
x=$(alias al_cs=true); alias al_cs >/dev/null 2>&1 && echo "alias: leaked" || echo "alias: kept inside"
x=$(export ex_cs=1); echo "export: [${ex_cs-unset}]"
x=$(umask 077); echo "umask: $(umask)"
x=$(shopt -s nullglob); shopt -q nullglob && echo "shopt: leaked" || echo "shopt: kept inside"
x=$(readonly ro_cs=1); ro_cs=2 2>/dev/null && echo "readonly: kept inside" || echo "readonly: leaked"
x=$(set -- a b c); echo "positional: $#"
x=$(exec 3>/dev/null); { echo >&3; } 2>/dev/null && echo "fd: leaked" || echo "fd: kept inside"
echo done
