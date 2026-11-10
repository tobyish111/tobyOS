# The prompt's \s: the last part of the name the shell was run as (bash's
# base_pathname(shell_name)) -- a login shell's `-` kept, the program's own
# name for an empty one or a lone `-` (not compared: that name is the
# shell's identity) -- and for `-c CMD NAME`, NAME. Oils 2015.
s() { ( exec -a "$1" "$BASH" -c 'PS1="<\s>"; echo "${PS1@P}"' ); echo "rc=$?"; }
s -bash
s /x/y/foo
s a/b/
s //z
s sh
s /
s 'we ird'
"$BASH" -c 'PS1="<\s>"; echo "${PS1@P}"' nm
"$BASH" -c 'PS1="<\s>"; echo "${PS1@P} $0"' /p/q/name a b
"$BASH" -c 'PS1="<\s>"; echo "${PS1@P}"' ''
( exec -a /x/myshell "$BASH" -c 'PS1="<\s>"; set -- a; echo "${PS1@P}"; f() { echo "${PS1@P}"; }; f' )
