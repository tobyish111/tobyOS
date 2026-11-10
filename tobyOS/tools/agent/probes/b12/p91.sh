exec 2>&1
coproc { echo to-stderr >&2; echo to-pipe; read -r _; }; read -r r <&"${COPROC[0]}"; echo "r=[$r]"; echo x >&"${COPROC[1]}"
wait
echo end
exec 3>&1 2>&3
coproc N2 { echo e2 >&2; echo o2; read -r _; }; read -r q <&"${N2[0]}"; echo "q=[$q]"; echo x >&"${N2[1]}"; wait
