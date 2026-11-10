# A command substitution is a subshell: nothing it changes reaches the shell
# that ran it (POSIX 2.6.3, 2.12). /bin/tsh runs it in its own process and
# let all of this out -- variables, the directory, functions, options,
# traps, exports, the umask, shopts, readonly, $@, descriptors. A ( )
# subshell kept most of it, but not a shopt or the umask.
cd /tmp; here=$PWD
x=$(y_cs=5); echo "variable: [${y_cs-unset}]"
x=$(cd /); echo "cwd: [$([ "$PWD" = "$here" ] && echo same || echo "$PWD")]"
x=$(f_cs() { echo f; }); type f_cs >/dev/null 2>&1 && echo "function: leaked" || echo "function: kept inside"
x=$(set -f); case $- in *f*) echo "option: leaked";; *) echo "option: kept inside";; esac
x=$(trap 'echo trapped' USR1); echo "trap: [$(trap -p USR1)]"
trap 'echo usr2' USR2; echo "the caller's trap, seen inside: [$(trap -p USR2)]"; trap - USR2
x=$(alias al_cs=true); alias al_cs >/dev/null 2>&1 && echo "alias: leaked" || echo "alias: kept inside"
x=$(export ex_cs=1); echo "export: [${ex_cs-unset}]"
old=$(umask); x=$(umask 077); echo "umask: $([ "$(umask)" = "$old" ] && echo same || umask)"
x=$(shopt -s nullglob); shopt -q nullglob && echo "shopt: leaked" || echo "shopt: kept inside"
x=$(readonly ro_cs=1); (ro_cs=2) 2>/dev/null && echo "readonly: kept inside" || echo "readonly: leaked"
x=$(set -- a b c); echo "positional: $#"
x=$(exec 3>/dev/null); { echo >&3; } 2>/dev/null && echo "fd: leaked" || echo "fd: kept inside"
x=$(echo out; y_cs2=in; echo "$y_cs2"); echo "output still captured: [$x] [${y_cs2-unset}]"
x=$(exit 7); echo "status of an assignment from it: $?"
( umask 077 ); echo "( umask ): $([ "$(umask)" = "$old" ] && echo same || umask)"
( shopt -s nullglob ); shopt -q nullglob && echo "( shopt ): leaked" || echo "( shopt ): kept inside"
echo done
