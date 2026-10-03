exec 2>&1
dead() { sleep 1 & k=$!; kill -9 $k; e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
dead; echo "P-before"; x=$(
echo p
); echo "P-after [$x]"
dead; dead; echo "Q-before"; x=$(echo q) y=$(echo q2
echo q3); echo "Q-after [$x] [$y]"
dead; echo "R-before"; echo "R" \
  $(echo r2); echo "R-after"
dead; echo "S-before"; x=$(echo $(echo s)); echo "S-after [$x]"
for i in 1 2; do
  dead; echo "T-before $i"; x=$(echo t); echo "T-after"
done
dead; echo "U-before"; case $(echo u) in u) echo "U-in";; esac; echo "U-after"
dead; echo "V-before"; echo v > /tmp/pr/p100.$(echo v); echo "V-after"
dead; echo "W-before"; x=$(echo w; echo w2); echo "W-after"
echo end
