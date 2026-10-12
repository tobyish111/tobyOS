#!/bin/bash
# suhost.sh -- build programs/user_shellutils for LINUX, in WSL, so the
# utilgate fixtures (logs/utilgate/) can be run against it and against
# GNU's tools on the same machine in seconds, before a guest is booted.
#
#     bash tools/agent/suhost.sh          # builds /tmp/suhost/bin/<name>...
#
# SU_HOST swaps the few tobyOS-only calls for Linux's (sync, setpriority);
# everything else is the same source the guest runs. On Linux it reads
# Linux's /proc, so free and pgrep meet procps on the same data.
cd "$(dirname "$0")/../.." || exit 1
MSYS_NO_PATHCONV=1 wsl.exe -e bash -c '
set -e
cd /mnt/c/CustomOS/tobyOS
mkdir -p /tmp/suhost/bin
gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -DSU_HOST \
    -o /tmp/suhost/shellutils programs/user_shellutils/main.c -lm
for n in rmdir yes tac seq whoami which uname free sync stat du xargs nohup \
         timeout watch dd printenv nice pgrep pkill tty stty cksum truncate \
         install mkfifo; do
    ln -sf /tmp/suhost/shellutils /tmp/suhost/bin/$n
done
echo "suhost: built /tmp/suhost/shellutils"
' 2>&1 | tr -d '\r'
