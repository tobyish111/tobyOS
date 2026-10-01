# sigreport.sh -- run with 2>&1: what does a shell print, and what is $?, when a
# job dies of each terminating signal? Background (kill + wait) and
# foreground (the child kills itself).
for s in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 24 25 26 27 29 30 31; do
  sleep 30 & p=$!; kill -$s $p; wait $p; echo "bg $s -> $?"
done
for s in 1 2 3 6 9 13 15; do
  sh -c "kill -$s \$\$"; echo "fg $s -> $?"
done
echo done
