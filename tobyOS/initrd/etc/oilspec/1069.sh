trap - XFSZ  # don't handle this

rm -f err.txt
touch err.txt

bytes() {
  local n=$1
  local st=0
  for i in $(seq $n); do
    echo -n x
    st=$?
    if test $st -ne 0; then
      echo "ERROR: echo failed with status $st" >> err.txt
    fi
  done
}

ulimit -f 1

bytes 512 > ok.txt
echo 512 status=$?

bytes 513 > too-big.txt
echo 513 status=$?
echo

wc --bytes ok.txt too-big.txt
echo

cat err.txt
