# POSIX 2.6.2 -- ${#p}, and the prefix/suffix removal family.
# The single/double distinction is shortest-match vs longest-match, which is
# how shells do dirname/basename/extension work without calling out.
p=/usr/local/share/doc/file.tar.gz
echo "len=${#p}"
echo "hash=${p#*/}"
echo "hashhash=${p##*/}"
echo "pct=${p%.*}"
echo "pctpct=${p%%.*}"
echo "dir=${p%/*}"
echo "base=${p##*/}"
echo "ext=${p##*.}"

f=aXbXcXd
echo "1=${f#*X}"
echo "2=${f##*X}"
echo "3=${f%X*}"
echo "4=${f%%X*}"

n=""
echo "empty-len=${#n}"
echo "nomatch=${p#zzz}"
echo "wholematch=[${f##*}]"

set -- one two three
echo "argc-len=${#}"
v=hello
echo "star-in-pattern=${v%l*}"
echo "quest-in-pattern=${v#?}"
echo end
