# bash's printf %(datefmt)T: strftime of an epoch-seconds operand. Fixed
# instants only, except the year -- which is the one clock reading that is
# safe to compare across the two runs, and the one that was WRONG: the
# native shell said 2025 on a machine whose RTC said 2026, because the
# native ABI had no realtime clock. bash (glibc, Linux personality) is the
# oracle for both the calendar and the formatting.
#
# Deliberately conservative on the conversions: %C %y %D %r %I %p %k %l and
# the very long single-line formats are libc-specific (the image's bash is
# musl-static and its strftime empties an over-long %()T buffer and drops a
# width on %_<n>d), so those are proven byte-for-byte against busybox in the
# distro gate's clock_fixed instead. Here we test the conversions that agree
# across libcs.
printf '%(%Y-%m-%d %H:%M:%S)T\n' 0
printf '%(%A %B %d %Y)T\n' 1700000000
printf '%(%s)T\n' 1700000000
printf '%(%j %U %W %V %G %g %u %w)T\n' 1700000000
printf '%(%a %b %e %Z %z)T\n' 1700000000
printf '%(%F %T %R)T\n' 1700000000
printf '[%20(%F)T][%-20(%F)T]\n' 86400 86400
printf '%()T\n' 0
printf '%(%Y)T\n' -1 | wc -c
printf '%(%s)T\n' -2 | wc -c
printf '%(%Y-%m-%d)T\n' -86400
printf '%(%V %G)T\n' 1609459200
printf '%(%V %G)T\n' 1640908800
printf '%(%V %G)T %(%j)T\n' 1704067200 1704067200
printf '%(%-d|%_H|%-m|%010Y)T\n' 1700000000
echo end
