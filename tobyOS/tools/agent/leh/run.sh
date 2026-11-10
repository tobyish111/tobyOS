#!/bin/bash
# run.sh [CASE...] -- type the same keys at GNU readline (WSL bash) and at
# programs/tsh/lineedit.c built on its own against stubs.c, each on a pty,
# and compare the line each accepts (driver.py). Needs WSL with bash, gcc
# and python3. "DIFFERENT: 0" is the pass.
cd "$(dirname "$0")" || exit 1
cp ../../../programs/tsh/lineedit.c ../../../programs/tsh/lineedit.h . || exit 1
mkdir -p tobyos && cp ../../../include/tobyos/wctab.h tobyos/ || exit 1
wsl -e bash -c 'cd "$(wslpath "'"$(pwd -W 2>/dev/null || pwd)"'")" && gcc -O1 -Wall -Wextra -I. -o /tmp/leh stubs.c lineedit.c && sed "s#\./leh#/tmp/leh#" driver.py > /tmp/leh_driver.py && timeout 1800 python3 /tmp/leh_driver.py '"$*"
rm -rf lineedit.c lineedit.h tobyos
