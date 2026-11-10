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
# And the box's wall clock within 5 s of this host's (finding 157), and its
# RTC within a second of that clock (finding 168, the write-back).
# A mark missed prints FAIL and the script exits 1. ED_HOST must be set.
cd "$(dirname "$0")/../.." || exit 1
export ED_HOST=${ED_HOST:?}
up() { bash logs/edremote.sh "cat > /tmp/$2; chmod +x /tmp/$2; md5sum /tmp/$2" < "$1" 2>/dev/null | tr -d '\r'; }
up logs/b26/killlat2 killlat2 > /dev/null
up logs/b27/sigdelay sigdelay > /dev/null
k=$(ED_TIMEOUT=300 bash logs/edremote.sh '/tmp/killlat2' 2>/dev/null | tr -d '\r')
s=$(ED_TIMEOUT=300 bash logs/edremote.sh '/tmp/sigdelay 20' 2>/dev/null | tr -d '\r')
bash logs/edremote.sh 'rm -f /tmp/killlat2 /tmp/sigdelay' > /dev/null 2>&1   # /tmp is 16 MB
printf '%s\n%s\n' "$k" "$s"
med() { printf '%s\n' "$1" | awk -v k="$2" '$1 == k { for (i = 2; i <= NF; i++) if ($i == "med") { print $(i + 1); exit } if ($2 ~ /^[0-9]+$/) { print $2; exit } }'; }
fe=$(med "$k" forkexit); ex=$(med "$k" exitself); sp=$(med "$s" spin)
rc=0
chk() { if [ -n "$2" ] && [ "$2" -le "$3" ]; then echo "  ok   $1 $2 us (mark $3)"; else echo "  FAIL $1 ${2:-?} us (mark $3)"; rc=1; fi; }
chk "fork+exit+reap" "$fe" 400
chk "exit to reap" "$ex" 300
chk "kill to SIGCHLD, busy parent" "$sp" 2000
# the wall clock against this machine's (finding 157: the box's RTC was 7
# hours behind UTC until timesyncd); the round trip is a second at most
hs=$(date +%s); bs=$(bash logs/edremote.sh 'date +%s' 2>/dev/null | tr -d '\r'); he=$(date +%s)
if [ -n "$bs" ] && [ "$bs" -ge $((hs - 5)) ] && [ "$bs" -le $((he + 5)) ]; then
  echo "  ok   wall clock within 5 s of this host's ($bs vs $hs)"
else
  echo "  FAIL wall clock ${bs:-?} vs this host's $hs"; rc=1
fi
# the RTC against the box's clock (finding 168: timesyncd writes it back
# after each sync; an Intel RTC's write restarts its divider, so the two
# agree to milliseconds -- hwclock prints the RTC's time at its own start,
# a few ms after date's). The mark is a second.
r=$(ED_USER=root bash logs/edremote.sh 'd=$(date -u +%s.%N); h=$(hwclock -r --utc); echo "$d|$h"' 2>/dev/null | tr -d '\r')
hr=$(date -d "${r#*|}" +%s.%N 2>/dev/null)
dm=$(awk -v a="$hr" -v b="${r%%|*}" 'BEGIN { if (a == "" || b == "") exit; d = (a - b) * 1000; if (d < 0) d = -d; printf "%d", d }')
if [ -n "$dm" ] && [ "$dm" -le 1000 ]; then
  echo "  ok   RTC within $dm ms of the box's clock (mark 1000; $r)"
else
  echo "  FAIL RTC ${dm:-?} ms from the box's clock ($r)"; rc=1
fi
echo "HWLATENCY: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
