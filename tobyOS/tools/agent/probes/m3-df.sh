# M3: what /bin/df prints, next to busybox df -- which reads the same
# filesystem numbers through Linux statfs(2) -- for /tmp, /, all mounts,
# -h, and a missing path.
for args in "/tmp" "-h /tmp" "/" "" "-h" "/nonexistent_m3"; do
  echo "=== df $args"
  /bin/df $args 2>&1; echo "rc=$?"
  echo "--- busybox df $args"
  busybox df $args 2>&1; echo "rc=$?"
done
