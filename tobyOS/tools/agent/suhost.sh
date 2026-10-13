#!/bin/bash
# suhost.sh -- build programs/user_shellutils for LINUX, in WSL, so the
# utilgate fixtures (logs/utilgate/) can be run against it and against
# GNU's tools on the same machine in seconds, before a guest is booted.
#
#     bash tools/agent/suhost.sh          # builds /tmp/suhost/bin/<name>...
#
# SU_HOST swaps the few tobyOS-only calls for Linux's (sync); everything
# else is the same source the guest runs. On Linux it reads Linux's /proc,
# so free and pgrep meet procps on the same data. libtoby's digests and
# monocypher are compiled in as the guest links them, with libtoby's
# headers searched after the C library's (for <toby/...> alone).
cd "$(dirname "$0")/../.." || exit 1
# pipefail: the tr below must not hide a failed compile -- and the old
# binary goes first, or a failed build leaves links to nothing and the
# fixtures quietly run the system's own free, pgrep... (they did once)
set -o pipefail
MSYS_NO_PATHCONV=1 wsl.exe -e bash -c '
set -e
cd /mnt/c/CustomOS/tobyOS
mkdir -p /tmp/suhost/bin
rm -f /tmp/suhost/shellutils
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -DSU_HOST \
    -idirafter libtoby/include -I third_party/monocypher-4.0.2/src \
    -o /tmp/suhost/shellutils programs/user_shellutils/main.c \
    libtoby/src/md5sha1.c libtoby/src/sha2.c \
    third_party/monocypher-4.0.2/src/monocypher.c -lm -lpthread
for n in rmdir yes tac seq whoami which uname free sync stat du xargs nohup \
         timeout watch dd printenv nice pgrep pkill tty stty cksum truncate \
         install mkfifo date touch sort script; do
    ln -sf /tmp/suhost/shellutils /tmp/suhost/bin/$n
done
echo "suhost: built /tmp/suhost/shellutils"
' 2>&1 | tr -d '\r'
