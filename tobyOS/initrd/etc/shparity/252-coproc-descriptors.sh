# A coprocess reads its own pipe, not the input of the command that
# started it, and its stderr stays the shell's after `exec 2>&1` and
# `exec 3>&1 2>&3`. Each coprocess waits for a line from the shell before
# it exits: bash disposes of a coprocess that has ended before it finishes
# starting it, so a `read` from one that exits at once is a race in bash
# itself (this fixture lost it once, in batch 12's final gate).
# (batch 12; probe p91 in C:\t\b12)
exec 2>&1
coproc { echo to-stderr >&2; echo to-pipe; read -r _; }; read -r r <&"${COPROC[0]}"; echo "r=[$r]"; echo x >&"${COPROC[1]}"
wait
echo end
exec 3>&1 2>&3
coproc N2 { echo e2 >&2; echo o2; read -r _; }; read -r q <&"${N2[0]}"; echo "q=[$q]"; echo x >&"${N2[1]}"; wait
