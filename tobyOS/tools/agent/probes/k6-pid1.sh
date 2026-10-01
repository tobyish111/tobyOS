# K6: is pid 1 ever handed out again? 400 short processes, then the pids.
n=0; ones=0
for i in $(seq 400); do p=$(sh -c 'echo $$'); n=$((n+1)); [ "$p" = 1 ] && ones=$((ones+1)); done
echo "processes=$n pid1=$ones"
