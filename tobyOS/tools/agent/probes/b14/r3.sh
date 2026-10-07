exec 2>&1 </dev/null
# rm -r removes a directory whatever its size: every entry, then itself
d=/data/r3.$$
mkdir -p "$d/sub" || exit 1
i=0
while [ $i -lt 150 ]; do : > "$d/f$i"; : > "$d/sub/g$i"; i=$((i+1)); done
ls "$d" | wc -l
ls "$d/sub" | wc -l
rm -r "$d"; echo "rm rc=$?"
[ -e "$d" ] && echo "still there" || echo "gone"
# find sees every entry of a directory it is not changing
mkdir -p "$d" && i=0
while [ $i -lt 40 ]; do : > "$d/h$i"; i=$((i+1)); done
find "$d" -type f | wc -l
rm -rf "$d"; echo "rm -rf rc=$?"
