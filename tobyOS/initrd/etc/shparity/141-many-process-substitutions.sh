cat <(echo 1) <(echo 2) <(echo 3) <(echo 4) <(echo 5) <(echo 6) <(echo 7) <(echo 8) <(echo 9) <(echo 10) <(echo 11) <(echo 12) | tr '\n' ' ' | sed 's/^/s1 /'; echo
paste <(echo a) <(echo b) <(echo c) <(echo d) <(echo e) <(echo f) <(echo g) <(echo h) <(echo i) <(echo j) | sed 's/^/s2 /'
echo done
