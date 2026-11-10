# A remembered path that is not there is reported as the path, "No such
# file or directory", status 127 -- a relative one too: `hash -p sh a; a`
# where there is no ./sh. tsh said "sh: command not found" (finding 580).
exec 2>&1
cd "$(mktemp -d)" || exit 1
PATH=/bin:/usr/bin
hash -p sh a; a -c 'echo from-a'; echo "rc=$?"
BASH_CMDS[d]=nosuch; d; echo "rc=$?"
hash -p /no/such/e e; e; echo "rc=$?"
