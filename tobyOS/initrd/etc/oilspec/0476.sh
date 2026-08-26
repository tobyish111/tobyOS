{ sleep 0.09; exit 9; } &
{ sleep 0.07; exit 7; } &
wait  # wait for all gives 0
echo "status=$?"
