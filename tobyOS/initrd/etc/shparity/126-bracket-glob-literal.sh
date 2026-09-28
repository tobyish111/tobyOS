d=${TMPDIR:-/tmp}/bg.$$
mkdir -p "$d" && cd "$d" || exit 1
touch '[' 'a[b' ab ac
echo 1: [
echo 2: a[b
echo 3: a[bc]
echo 4: a[!b]
echo 5: []
echo 6: [!]
echo 7: a[]]
echo 8: *[
i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo "9: i=$i"
cd / && rm -rf "$d"
echo done
