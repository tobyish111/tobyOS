# noclob.sh: noclobber against what is not a regular file (Oils 2134) --
# each redirection under set -C, its status, in a subshell of its own; a
# symlink to a file, to /dev/null, a dangling one, a directory, a FIFO.
exec 2>&1
N() { sed "s#/tmp/nc\.[0-9]*#/tmp/nc.N#g; s/^[^:]*: line [0-9]*: //"; }
for r in '>' '&>' '>>' '&>>' '>|' '>&'; do
  ( set -C; eval "echo a $r /dev/null" ) 2>&1 | N; echo "null $r st=${PIPESTATUS[0]}"
done
for r in '>' '>>'; do
  ( set -C; eval "echo a $r /dev/zero" ) 2>&1 | N; echo "zero $r st=${PIPESTATUS[0]}"
done
d=/tmp/nc.$$; mkdir -p $d; echo x > $d/f
( set -C; echo a > $d/f ) 2>&1 | N; echo "regular st=${PIPESTATUS[0]} [$(cat $d/f)]"
( set -C; echo a &> $d/f ) 2>&1 | N; echo "regular &> st=${PIPESTATUS[0]} [$(cat $d/f)]"
( set -C; echo a >& $d/f ) 2>&1 | N; echo "regular >& st=${PIPESTATUS[0]} [$(cat $d/f)]"
( set -C; echo a >> $d/f ) 2>&1 | N; echo "regular >> st=${PIPESTATUS[0]} [$(echo $(cat $d/f))]"
ln -s $d/f $d/lf; ( set -C; echo a > $d/lf ) 2>&1 | N; echo "link-to-file st=${PIPESTATUS[0]} [$(cat $d/f)]"
ln -s /dev/null $d/ln; ( set -C; echo a > $d/ln ) 2>&1 | N; echo "link-to-null st=${PIPESTATUS[0]}"
ln -s $d/none $d/dl; ( set -C; echo a > $d/dl ) 2>&1 | N; echo "dangling st=${PIPESTATUS[0]} $([ -e $d/none ] && echo created || echo absent) $([ -L $d/dl ] && echo still-a-link)"
( set -C; echo a > $d ) 2>&1 | N; echo "dir st=${PIPESTATUS[0]}"
( set -C; echo a > $d/new ) 2>&1 | N; echo "new st=${PIPESTATUS[0]} [$(cat $d/new)]"
# a reader that never gets a writer is killed (a shell that refuses before
# opening leaves it waiting); the job notice goes nowhere
mkfifo $d/p; exec 3>&1
{ cat $d/p > $d/got & c=$!
  ( set -C; echo viafifo > $d/p ) 2>&1 | N >&3; echo "fifo st=${PIPESTATUS[0]}" >&3
  sleep 1; kill $c; wait; } 2>/dev/null
exec 3>&-; echo "fifo got [$(cat $d/got)]"
rm -rf $d
( set -C; set -e; echo a > /dev/null; echo a &> /dev/null; echo a >> /dev/null; echo a &>> /dev/null; echo a >| /dev/null; echo done ); echo "case st=$?"
echo end
