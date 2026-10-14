#!/bin/bash
# hwlatency.sh -- process and signal latency on the box, against fixed marks
# Linux meets. Two static Linux probes (built with gcc -O2 -static in WSL;
# sources beside them) are uploaded and run as the user:
#   logs/b26/killlat2 -- fork, _exit and reap; a child's last timestamp to its
#                        parent's reap (finding 156: 800 and 497 us when every
#                        logged byte cost three port accesses; Linux 138, 103);
#   logs/b27/sigdelay -- kill to the SIGCHLD handler of a parent busy in user
#                        code (finding 165: one 10 ms timer tick before the
#                        signal kick; Linux a few hundred us).
# A mark missed prints FAIL and the script exits 1. ED_HOST must be set.
cd "$(dirname "$0")/../.." || exit 1
export ED_HOST=${ED_HOST:?}
up() { bash logs/edremote.sh "cat > /tmp/$2; chmod +x /tmp/$2; md5sum /tmp/$2" < "$1" 2>/dev/null | tr -d '\r'; }
up logs/b26/killlat2 killlat2 > /dev/null
up logs/b27/sigdelay sigdelay > /dev/null
k=$(ED_TIMEOUT=300 bash logs/edremote.sh '/tmp/killlat2' 2>/dev/null | tr -d '\r')
s=$(ED_TIMEOUT=300 bash logs/edremote.sh '/tmp/sigdelay 20' 2>/dev/null | tr -d '\r')
printf '%s\n%s\n' "$k" "$s"
med() { printf '%s\n' "$1" | awk -v k="$2" '$1 == k { for (i = 2; i <= NF; i++) if ($i == "med") { print $(i + 1); exit } if ($2 ~ /^[0-9]+$/) { print $2; exit } }'; }
fe=$(med "$k" forkexit); ex=$(med "$k" exitself); sp=$(med "$s" spin)
rc=0
chk() { if [ -n "$2" ] && [ "$2" -le "$3" ]; then echo "  ok   $1 $2 us (mark $3)"; else echo "  FAIL $1 ${2:-?} us (mark $3)"; rc=1; fi; }
chk "fork+exit+reap" "$fe" 400
chk "exit to reap" "$ex" 300
chk "kill to SIGCHLD, busy parent" "$sp" 2000
echo "HWLATENCY: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
