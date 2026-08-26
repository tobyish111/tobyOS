# note: AT&T ksh supports this too

case $SH in dash|ash) exit ;; esac

show_bytes() {
  # -A n - no file offset
  od -A n -t c -t x1
}

# this isn't special
# mksh doesn't like it
#echo -n $'\c' | show_bytes

echo -n $'\c0\c9-' | show_bytes
echo

# control chars are case insensitive
echo -n $'\ca\cz' | show_bytes
echo

echo -n $'\cA\cZ' | show_bytes
echo

echo -n $'\c-\c+\c"' | show_bytes
