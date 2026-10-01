# S8: sh is /bin/sh, not a builtin; and what enable lists.
type sh; echo "type: $?"
command -V sh; echo "command -V: $?"
type -t sh
p=$$; c=$(sh -c 'echo $$'); [ "$c" != "$p" ] && echo "sh -c: own pid" || echo "sh -c: caller's pid"
enable | grep -c .
enable | grep -w -e sh -e cat -e ls -e mkdir | head
enable sh; echo "enable sh: $?"
enable -n sh; echo "enable -n sh: $?"
help sh >/dev/null 2>&1; echo "help sh: $?"
compgen -b | grep -c .
compgen -b | grep -x sh
echo done
