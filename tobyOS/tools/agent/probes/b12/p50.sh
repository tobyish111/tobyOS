echo x >&-; echo "1 s=$?" >&2
( exec >&-; echo y; echo "2 s=$?" >&2 )
{ echo z; } >&-; echo "3 s=$?" >&2
printf 'w\n' >&-; echo "4 s=$?" >&2
f() { echo f; }; f >&-; echo "5 s=$?" >&2
echo hi > /dev/full; echo "6 s=$?" >&2
