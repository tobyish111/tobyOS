i=0
while [ $i -lt 300 ]; do cat <(echo $i) <(echo x) >/dev/null; i=$((i+1)); done
echo "loop $i"
cat <(echo 1) <(echo 2) <(echo 3) | tr '\n' ' '; echo
x=$(cat <(echo in) <(echo sub)); echo "c [$x]"
echo done
