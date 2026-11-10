exec 2>&1
sleep 1 & k=$!; kill -9 $k; sleep 0.2; echo "v4 fg external"
sleep 1 & k=$!; kill -9 $k; x=$(echo in); echo "v1 [$x]"
sleep 1 & k=$!; kill -9 $k; x=$(sleep 0.2; echo in); echo "v2 [$x]"
sleep 1 & k=$!; kill -9 $k; x=$(/bin/sleep 0.2); echo "v3 [$x]"
sleep 1 & k=$!; kill -9 $k; ( sleep 0.2; echo "v5 sub" ); echo "v5 after"
sleep 1 & k=$!; kill -9 $k; ( /bin/sleep 0.2 ); echo "v5b after"
sleep 1 & k=$!; kill -9 $k; /bin/sleep 0.2 | cat; echo "v7 pipe after"
sleep 1 & k=$!; kill -9 $k; f() { sleep 0.2; }; f; echo "v8 fn after"
sleep 1 & k=$!; kill -9 $k; true; echo "v6 builtin"
echo "v6 next"
sleep 1 & k=$!; kill -9 $k; sleep 0.2 & echo "v9 bg"; wait $!; echo "v9 waited"
echo end
