exec 2>&1
busy() { e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
sleep 1 & kill -9 $!; busy; { sleep 0.3; echo x1; } | cat; echo "A"
sleep 1 & kill -9 $!; busy; ( sleep 0.3; echo x2 ); echo "B"
sleep 1 & kill -9 $!; busy; { sleep 0.3; echo x3; } & wait; echo "C"
sleep 1 & kill -9 $!; busy; cat <(sleep 0.3; echo x4); echo "D"
( sleep 1 & kill -9 $!; sleep 0.3; echo "F own" ); echo "F after"
{ sleep 1 & kill -9 $!; sleep 0.3; echo "G own"; } | cat; echo "G after"
{ sleep 1 & kill -9 $!; sleep 0.3; echo "H own"; } & wait; echo "H after"
cat <(sleep 1 & kill -9 $!; sleep 0.3; echo "I own"); echo "I after"
x=$(sleep 1 & kill -9 $!; sleep 0.3; echo "J own"); echo "J after [$x]"
coproc { sleep 1 & kill -9 $!; sleep 0.3; echo "K own"; }; read -r l <&"${COPROC[0]}"; echo "K after [$l]"; wait
echo end
