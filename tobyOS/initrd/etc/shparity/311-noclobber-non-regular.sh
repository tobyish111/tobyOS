# NOCLOBBER REFUSES A REGULAR FILE, NOT EVERY NAME THAT IS THERE: under
# set -C, `>`, `&>` and `>&` onto /dev/null, /dev/zero, a link to /dev/null
# and a FIFO write as bash does; a regular file (through a link too) and a
# dangling link are refused; a directory is "Is a directory". tsh refused
# every existing name and its `>&` truncated; the kernel's FIFO fstat and
# its open through a link to a device made the guest's bash refuse too
# (findings 113 106; Oils 2134; logs/b18 noclob symdev).
exec 2>&1
N() { sed "s#/tmp/nc\.[0-9]*#/tmp/nc.N#g; s/^[^:]*: line [0-9]*: //"; }
for r in '>' '&>' '>>' '&>>' '>|' '>&'; do
  ( set -C; eval "echo a $r /dev/null" ) 2>&1 | N; echo "null $r st=${PIPESTATUS[0]}"
done
( set -C; echo a > /dev/zero ) 2>&1 | N; echo "zero st=${PIPESTATUS[0]}"
d=/tmp/nc.$$; mkdir -p $d; echo x > $d/f
for r in '>' '&>' '>&'; do
  ( set -C; eval "echo a $r $d/f" ) 2>&1 | N; echo "regular $r st=${PIPESTATUS[0]} [$(cat $d/f)]"
done
( set -C; echo a >> $d/f ) 2>&1 | N; echo "regular >> st=${PIPESTATUS[0]} [$(echo $(cat $d/f))]"
ln -s $d/f $d/lf; ( set -C; echo b > $d/lf ) 2>&1 | N; echo "link-to-file st=${PIPESTATUS[0]}"
ln -s /dev/null $d/ln; ( set -C; echo a > $d/ln ) 2>&1 | N; echo "link-to-null st=${PIPESTATUS[0]}"
read -r l < $d/ln; echo "read link-to-null st=$? [$l]"
ln -s $d/none $d/dl; ( set -C; echo a > $d/dl ) 2>&1 | N
echo "dangling st=${PIPESTATUS[0]} $([ -e $d/none ] && echo created || echo absent) $([ -L $d/dl ] && echo still-a-link)"
( set -C; echo a > $d ) 2>&1 | N; echo "dir st=${PIPESTATUS[0]}"
( set -C; echo a > $d/new ) 2>&1 | N; echo "new st=${PIPESTATUS[0]} [$(cat $d/new)]"
mkfifo $d/p; exec 3<>$d/p
( set -C; echo viafifo > $d/p ) 2>&1 | N; echo "fifo st=${PIPESTATUS[0]}"
read -t 3 -r l <&3; echo "fifo got [$l]"
exec 3<&-
rm -rf $d
( set -C; set -e; echo a > /dev/null; echo a &> /dev/null; echo a >> /dev/null; echo a &>> /dev/null; echo a >| /dev/null; echo done ); echo "case st=$?"
echo end
