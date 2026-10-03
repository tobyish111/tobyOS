exec 2>&1
f() {
  echo "in f"
  sh -c 'kill -9 $$'
  echo "after kill in f"
}
f
for i in 1; do
  echo "in for"
  sh -c 'kill -9 $$'
done
k=0
while (( k < 1 )); do
  sh -c 'kill -9 $$'
  k=1
done
sleep 1 & kill -9 $!; sleep 0.3; sh -c 'kill -9 $$'; echo "both"
g() { sleep 1 & kill -9 $!; sleep 0.3; sh -c 'kill -9 $$'; }
g
echo end
