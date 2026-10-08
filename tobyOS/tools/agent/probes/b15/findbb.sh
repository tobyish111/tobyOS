# findbb.sh: the native find against busybox's, on one tree, for the
# expressions both know (item 7). Output sorted: readdir order is the
# filesystem's, and the two may walk differently.
exec 2>&1
T=/tmp/fbb; rm -rf $T; mkdir -p $T/a/b/c $T/d $T/empty
cd $T
printf x > a/one; printf '%0600d' 0 > a/b/big; : > a/b/zero
printf hello > d/f1; printf hi > d/F2.TXT; printf z > a/b/c/deep.txt
ln -s ../d/f1 a/link1; ln -s nosuch a/dangling; chmod 755 a/one; chmod 600 d/f1
for e in "" "-name f1" "-iname 'f2*'" "-name '*.txt'" "-path './a/*'" \
         "-type f" "-type d" "-type l" "-maxdepth 1" "-mindepth 2" \
         "-size 0" "-size +1" "-size -2" "-empty" "-perm 600" "-perm -644" \
         "-name b -prune -o -print" "! -name '*.txt' -type f" \
         "\\( -name f1 -o -name one \\)" "-type f -exec echo X {} \\;" \
         "-newer a/one -type f" "-links 1 -type d" "-depth" "-L -type f"; do
  case "$e" in -L*) o=-L; e=${e#-L }; ;; *) o=; ;; esac
  n=$(eval "/bin/find $o . $e" | sort | tr '\n' ' ')
  b=$(eval "busybox find $o . $e" | sort | tr '\n' ' ')
  if [ "$n" = "$b" ]; then echo "same  $o $e"; else echo "DIFF  $o $e"; echo "  find:    $n"; echo "  busybox: $b"; fi
done
/bin/find nosuch; echo "rc=$?"
/bin/find . -foo; echo "rc=$?"
/bin/find . -print0 | tr '\0' '\n' | sort | head -3
cd /; rm -rf $T
echo end
