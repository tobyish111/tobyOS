exec 2>&1
dead() { sleep 1 & k=$!; kill -9 $k; e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
dead; x=$(echo a); echo "a1"
dead; x=$(echo a; echo b); echo "a2"
dead; x=$(echo a;); echo "a3"
dead; x=$(echo a
); echo "a4"
dead; echo $(echo a) >/dev/null; echo "a5"
dead; x=$(echo a) >/dev/null; echo "a6"
dead; x=$(echo a) && echo "a7"
dead; x=$(echo a) ; echo "a8"
dead; x=$(echo a)
echo "a9"
dead; x=$(echo a b c)
echo "a10"
dead; x=$(echo a|cat)
echo "a11"
dead; x=$( echo a )
echo "a12"
dead; x=$(true)
echo "a13"
dead; x=$(:)
echo "a14"
echo end
