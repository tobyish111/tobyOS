# NAMED PIPES. mkfifo makes one, `test -p` and `ls -l`/`ls -F` see one, a
# writer meets a reader in either order, `exec 8<> fifo` is both ends on
# one descriptor (Oils 2126), the reader sees EOF when the writer goes, and
# `find -type p` finds one. There were none: mkfifo refused, mknod S_IFIFO
# was EPERM, and tsh opened `<` read-write, so a reader never saw EOF
# (finding 96; logs/b16 fifo).
exec 2>&1
d=${TMP:-/tmp}/fx297.$$; rm -rf "$d"; mkdir -p "$d"; cd "$d" || exit 1
mkfifo p1; echo "f1 mk=$?"
[ -p p1 ] && echo "f2 p1 is a fifo"; [ -f p1 ] && echo "f2 p1 is a file" || echo "f2 p1 is not a file"
echo "f3 $(ls -l p1 | cut -c1) $(ls -F | tr '\n' ' ')"
mkfifo p1 2>/dev/null; echo "f4 again=$?"
mkfifo -m 600 p2; echo "f5 $(ls -l p2 | cut -c1-10)"
( echo hello > p1 ) & cat p1; wait; echo "f6 writer first"
cat p1 & echo world > p1; wait; echo "f7 reader first"
exec 8<> p1; echo first >&8; echo second >&8; read a <&8; read b <&8; exec 8<&-
echo "f8 $a $b"
( for i in 1 2 3; do echo "line $i"; done > p1 ) & while read x; do echo "f9 $x"; done < p1; wait
( exec 3> p1; echo one >&3; exec 3>&- ) & read l < p1; echo "f10 l=$l"; wait
echo "f11 $(find . -type p | sort | tr '\n' ' ')"
echo "f12 $(find . -type f | wc -l)"
( sleep 0.2; echo late > p1 ) & read v < p1; echo "f13 $v"; wait
rm p1 p2; echo "f14 $(ls | wc -l)"
cd /; rm -rf "$d"
echo end
