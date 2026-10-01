# What a ( ) subshell changes must not reach the shell that
# ran it -- it is a subshell (POSIX 2.6.3, 2.12). One line per kind of state.
cd /tmp; here=$PWD
( y_cs=5 ); echo "variable: [${y_cs-unset}]"
( cd / ); echo "cwd: [$([ "$PWD" = "$here" ] && echo same || echo "$PWD")]"
( f_cs() { echo f; } ); type f_cs >/dev/null 2>&1 && echo "function: leaked" || echo "function: kept inside"
( set -f ); case $- in *f*) echo "option: leaked";; *) echo "option: kept inside";; esac
( trap 'echo trapped' USR1 ); echo "trap: [$(trap -p USR1)]"
( alias al_cs=true ); alias al_cs >/dev/null 2>&1 && echo "alias: leaked" || echo "alias: kept inside"
( export ex_cs=1 ); echo "export: [${ex_cs-unset}]"
( umask 077 ); echo "umask: $(umask)"
( shopt -s nullglob ); shopt -q nullglob && echo "shopt: leaked" || echo "shopt: kept inside"
( readonly ro_cs=1 ); ro_cs=2 2>/dev/null && echo "readonly: kept inside" || echo "readonly: leaked"
( set -- a b c ); echo "positional: $#"
( exec 3>/dev/null ); { echo >&3; } 2>/dev/null && echo "fd: leaked" || echo "fd: kept inside"
echo done
