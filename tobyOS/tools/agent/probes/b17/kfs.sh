# kfs.sh: two kernel findings, measured from a shell (bash and tsh print the
# same). 55: /tmp's tmpfs held at most 512 nodes -- "No space left on
# device" at the 498th file with its blocks free. 32: every mount but
# tobyfs and tmpfs reported the root's device, so busybox `df /proc` named
# "/" and nothing could tell /proc, /dev and /sys from /.
exec 2>&1
d=/tmp/kfs.$$
mkdir "$d" || exit 1
n=0; fail=
while [ $n -lt 3000 ]; do
  : > "$d/f$n" || { fail=$n; break; }
  n=$((n + 1))
done
echo "55 created=$n fail=${fail:-none}"
echo "55 listed=$(ls "$d" | wc -l)"
ln "$d/f2999" "$d/link" && [ "$d/f2999" -ef "$d/link" ] && echo "55 link-ef ok"
echo data > "$d/f2500"; echo "55 read=$(cat "$d/f2500")"
rm -rf "$d"; echo "55 removed=$([ -e "$d" ] && echo no || echo yes)"
for m in / /proc /dev /sys /tmp /data /dev/shm; do
  [ -e "$m" ] && echo "32 $m dev=$(busybox stat -c %d "$m")"
done
echo "32 distinct=$(for m in / /proc /dev /sys /tmp; do busybox stat -c %d "$m"; done | sort -u | wc -l)/5"
echo "32 same-fs=$([ "$(busybox stat -c %d /proc/self)" = "$(busybox stat -c %d /proc)" ] && echo yes || echo no)"
busybox df /proc 2>&1 | tail -1 | awk '{print "32 df-/proc mounted-on=" $NF}'
busybox df /tmp 2>&1 | tail -1 | awk '{print "32 df-/tmp mounted-on=" $NF}'
busybox df /bin/tsh 2>&1 | tail -1 | awk '{print "32 df-/bin/tsh mounted-on=" $NF}'
echo end
