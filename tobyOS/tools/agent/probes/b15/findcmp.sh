# findcmp.sh MYFIND -- run a list of find expressions with GNU find and
# MYFIND over the same tree, in the C locale, and report any difference.
export LC_ALL=C
MY=$1
T=/tmp/ftree.$$
rm -rf $T; mkdir -p $T/a/b/c $T/d $T/empty $T/sp\ ace
cd $T
printf 'x' > a/one; printf '%0600d' 0 > a/b/big; : > a/b/zero
printf hello > d/f1; printf hi > d/F2.TXT; printf z > a/b/c/deep.txt
ln -s ../d/f1 a/link1; ln -s nosuch a/dangling; ln -s ../d a/dirlink
chmod 755 a/one; chmod 600 d/f1; chmod 4755 d/F2.TXT; chmod 640 a/b/zero
touch -d '2020-01-01' d/f1; touch -d '2001-06-01' a/one
cases=(
  ""
  "."
  ". -name f1"
  ". -iname 'f2*'"
  ". -name '*.txt'"
  ". -name '[a-d]'"
  ". -name '[!a]'"
  ". -path './a/*'"
  ". -ipath '*B*'"
  ". -type f"
  ". -type d"
  ". -type l"
  ". -type f,l"
  ". -xtype l"
  ". -xtype d"
  ". -maxdepth 1"
  ". -mindepth 2"
  ". -mindepth 1 -maxdepth 2 -type d"
  ". -size 0"
  ". -size +1"
  ". -size -2"
  ". -size 600c"
  ". -size +1k"
  ". -empty"
  ". -perm 600"
  ". -perm -644"
  ". -perm /4000"
  ". -perm -u=x -type f"
  ". -newer a/one -type f"
  ". -mtime +1000"
  ". -mtime -1"
  ". -links 1 -type d"
  ". -name b -prune -o -print"
  ". -name b -prune -o -type f -print"
  ". ! -name '*.txt' -type f"
  ". -not -type d"
  ". \\( -name f1 -o -name one \\)"
  ". -name f1 -o -name one"
  ". -type f -name 'f*' -o -name one -print"
  ". -print0"
  ". -type f -printf '%p %s %m %y %d %f %h\\n'"
  ". -depth"
  ". -depth -type d"
  ". -quit"
  ". -type f -exec echo X {} \\;"
  ". -type f -exec echo X {} +"
  ". -name 'f*' -exec echo '{}.bak' \\;"
  ". -type f -exec false \\; -o -print"
  "a d"
  "a/ d/"
  "nosuch"
  ". nosuch"
  "-L . -type l"
  "-L . -name f1"
  "-H a/link1"
  "a/link1"
  ". -readable -type f -name one"
  ". -executable -type f"
  ". -empty -type d -delete -o -print"
  ". -foo"
  ". -name"
  ". -name x y"
  ". \\( \\)"
  ". -maxdepth x"
  ". -type q"
  ". -size 3q"
  ". -name x -o"
  ". -name x -a"
  ". -name x \\)"
  ". -perm 9"
  ". -user nosuchuser"
  ". -links x"
  ". -samefile a/one"
  ". -inum 0"
)
for c in "${cases[@]}"; do
  g=$(eval "find $c" 2>&1 | od -c | head -60; echo "rc=${PIPESTATUS[0]}")
  rm -rf $T/empty 2>/dev/null; mkdir -p $T/empty
  m=$(eval "$MY $c" 2>&1 | sed 's/^[^:]*myfind: /find: /' | od -c | head -60; echo "rc=${PIPESTATUS[0]}")
  rm -rf $T/empty 2>/dev/null; mkdir -p $T/empty
  if [ "$g" = "$m" ]; then echo "SAME   find $c"; else echo "DIFF   find $c"; diff <(eval "find $c" 2>&1) <(eval "$MY $c" 2>&1) | head -10; mkdir -p $T/empty; fi
done
cd /; rm -rf $T
