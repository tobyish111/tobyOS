#!/bin/bash
# ttin.sh -- SIGTTIN for a background read of the controlling terminal
# (finding 89), in an SSH LOGIN session (`ssh -tt`): the only place a shell
# is a session leader with the pty as its terminal -- /bin/ttyparity's
# shells are not, so that gate cannot see it. Each shell is exec'd in turn
# into the session and runs the same lines: `cat &` must stop at once, and
# `kill -9` must kill the stopped job ("Killed"), not let it see EINTR.
# Usage: ED_HOST=127.0.0.1 ED_PORT=2222 bash tools/agent/probes/b15/ttin.sh OUTPREFIX
cd "$(dirname "$0")/../../../.." || exit 1
. logs/winenv.sh
OUT=${1:-logs/b15/ttin}
S="${SSH_BIN:-/c/Windows/System32/OpenSSH/ssh.exe}"
for sh in tsh bash; do
  case $sh in
    bash) start='exec bash --norc --noediting -i' ;;
    tsh)  start='exec tsh --noediting' ;;
  esac
  printf '%s\nPS1="P> "\ncat &\nsleep 1\njobs\nkill -9 %%1\nsleep 1\njobs\n( cat; echo "rc=$?" ) &\nsleep 1\njobs\nfg\n' "$start" > "$OUT.in"
  printf 'x\n' >> "$OUT.in"
  printf 'sleep 1\necho done\nexit\n' >> "$OUT.in"
  MSYS_NO_PATHCONV=1 timeout 60 "$S" -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=NUL -o LogLevel=ERROR -p "${ED_PORT:-22}" \
      -tt "root@${ED_HOST:?}" < "$OUT.in" 2>&1 | tr -d '\r' |
      sed -n '/^P> /,$p' | sed 's/\[[0-9]*\] [0-9][0-9]*/[N] PID/' > "$OUT.$sh"
done
rm -f "$OUT.in"
cmp -s "$OUT.tsh" "$OUT.bash" && echo "ttin IDENTICAL" || { echo "ttin DIFFERENT"; diff "$OUT.bash" "$OUT.tsh"; }
cat "$OUT.bash"
