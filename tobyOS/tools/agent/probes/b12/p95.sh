exec 2>&1
sleep 1 & k=$!; kill -9 $k; { x=$(echo in); } 2>/tmp/pr/e1; echo "[$x]"
echo next
echo "e1=[$(sed 's/[0-9][0-9]* Killed/PID Killed/' /tmp/pr/e1)]"
sleep 1 & k=$!; kill -9 $k; { x=$(echo in2); } 2>/tmp/pr/e2; echo "[$x]"; { true; } 2>/tmp/pr/e3
echo next2
echo "e2=[$(sed 's/[0-9][0-9]* Killed/PID Killed/' /tmp/pr/e2)]"
echo end
