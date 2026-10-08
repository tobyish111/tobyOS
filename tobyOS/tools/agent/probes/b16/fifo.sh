# fifo.sh: named pipes (finding 96) -- mkfifo, what stat and ls say of
# one, a writer meeting a reader in either order, `exec 8<> fifo` (both
# ends on one descriptor, Oils 2126), EOF when the writer goes, find -type
# p, rm. There were none: mkfifo refused and mknod S_IFIFO was EPERM.
exec 2>&1
cd /tmp; rm -rf fft; mkdir fft; cd fft
mkfifo p1; echo "f1 mk=$?"
[ -p p1 ] && echo "f2 p1 is a fifo"; [ -f p1 ] && echo "f2 p1 is a file" || echo "f2 p1 is not a file"
echo "f3 $(ls -l p1 | cut -c1) $(ls -F | tr '\n' ' ')"
mkfifo p1 2>/dev/null; echo "f4 again=$?"
mkfifo -m 600 p2; echo "f5 $(ls -l p2 | cut -c1-10)"
( echo hello > p1 ) & cat p1; wait; echo "f6 writer first"
cat p1 & sleep 0.5; echo world > p1; wait; echo "f7 reader first"
exec 8<> p1; echo first >&8; echo second >&8; read a <&8; read b <&8; exec 8<&-
echo "f8 $a $b"
( for i in 1 2 3; do echo "line $i"; done > p1 ) & while read x; do echo "f9 $x"; done < p1; wait
( exec 3> p1; echo one >&3; exec 3>&- ) & read l < p1; echo "f10 l=$l"; wait
echo "f11 $(find . -type p | sort | tr '\n' ' ')"
echo "f12 $(find . -type f | wc -l)"
( sleep 0.3; echo late > p1 ) & read v < p1; echo "f13 $v"; wait
rm p1 p2; echo "f14 $(ls | wc -l)"
cd /tmp; rm -rf fft
echo end
