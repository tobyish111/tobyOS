# N>&M- moves a descriptor; a write to a closed stdout is "write error:
# Bad file descriptor"; a read from a closed one is "read error: 0: ...";
# a program run with `<&-` gets fd 0 closed, not the shell's.
# (batch 12; probe p50 in C:\t\b12)
exec 2>&1
echo x >&-; echo "1 s=$?" >&2
( exec >&-; echo y; echo "2 s=$?" >&2 )
{ echo z; } >&-; echo "3 s=$?" >&2
printf 'w\n' >&-; echo "4 s=$?" >&2
f() { echo f; }; f >&-; echo "5 s=$?" >&2
echo hi > /dev/full; echo "6 s=$?" >&2
