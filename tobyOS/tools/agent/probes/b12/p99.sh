exec 2>&1
dead() { sleep 1 & k=$!; kill -9 $k; e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
dead; echo "A-before"; x=$(echo a); echo "A-after [$x]"
dead; echo "B-before"; x=$(echo b
echo b2); echo "B-after [$x]"
dead; echo "C-before"; x=`echo c`; echo "C-after [$x]"
dead; echo "D-before"; eval 'echo d'; echo "D-after"
dead; echo "E-before"; eval $'echo e1\necho e2'; echo "E-after"
dead; echo "F-before"; x="$(echo f)"; echo "F-after [$x]"
dead; echo "G-before"; cat <(echo g); echo "G-after"
dead; echo "H-before"; echo 'echo h' > /tmp/pr/p99src.sh; . /tmp/pr/p99src.sh; echo "H-after"
dead; echo "I-before"; trap 'echo itrap' USR1; kill -USR1 $$; echo "I-after"
f() {
  dead; echo "J-before"; x=$(echo j); echo "J-after"
}
f
dead; echo "K-before"; x=$(( 1 + 2 )); echo "K-after"
dead; echo "L-before"; [[ $(echo l) ]]; echo "L-after"
dead; echo "M-before"; for v in $(echo m); do echo "M-in $v"; done; echo "M-after"
dead; echo "N-before"; echo ${x:-$(echo n)}; echo "N-after"
dead; echo "O-before"; true; echo "O-after"
echo end
