#!/bin/bash
# ttou.sh -- SIGTTOU at a controlling terminal (POSIX XBD 11.1.4), in an
# SSH LOGIN session (`ssh -tt`): the session leader's pty is its terminal,
# which /bin/ttyparity's shells do not have. Each shell is exec'd into the
# session and fed the same lines on a timer:
#   u1  ^Z on a foreground job, and the shell takes its terminal back --
#       from the background, which sends SIGTTOU unless the shell blocks it
#   u2  under `stty tostop` a background job's write stops it
#   u3  a background `stty -echo` (a change to the terminal) stops it
# Usage: ED_HOST=... [ED_PORT=...] bash tools/agent/probes/b16/ttou.sh OUTPREFIX
cd "$(dirname "$0")/../../../.." || exit 1
. logs/winenv.sh
OUT=${1:-logs/b16/ttou}
S="${SSH_BIN:-/c/Windows/System32/OpenSSH/ssh.exe}"
feed() {
  local start=$1 l
  printf '%s\nPS1="P> "\n' "$start"; sleep 2
  printf 'sleep 30\n'; sleep 2; printf '\032'; sleep 1
  # one line a second, as typed: type-ahead's echo races the prompt
  for l in 'echo u1 regained' 'jobs' 'kill -9 %1' \
           'busybox stty tostop; (sleep 1; echo u2-out) &' '' '' \
           'jobs' 'busybox stty -tostop' 'fg' '' \
           'busybox stty -echo &' '' 'jobs' 'kill -9 %1' '' \
           'busybox stty echo; echo u4 done' 'exit'; do
    [ -n "$l" ] && printf '%s\n' "$l"
    sleep 1
  done
  sleep 1
}
for sh in tsh bash; do
  case $sh in
    bash) start='exec bash --norc --noediting -i' ;;
    tsh)  start='exec tsh --noediting' ;;
  esac
  feed "$start" | MSYS_NO_PATHCONV=1 timeout 60 "$S" -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=NUL -o LogLevel=ERROR -p "${ED_PORT:-22}" \
      -tt "root@${ED_HOST:?}" 2>&1 | tr -d '\r' |
      sed -n '/^P> /,$p' | sed -E 's/\[[0-9]+\] [0-9]+/[N] PID/; s/^P> (bash|tsh): /P> SH: /' > "$OUT.$sh"
done
cmp -s "$OUT.tsh" "$OUT.bash" && echo "ttou IDENTICAL" || { echo "ttou DIFFERENT"; diff "$OUT.bash" "$OUT.tsh"; }
cat "$OUT.bash"
