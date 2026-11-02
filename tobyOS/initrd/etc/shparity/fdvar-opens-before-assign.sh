# `{name}>file` opens the file before the name takes its number: with the
# name readonly the file is still made, or emptied, and when the open
# fails the name keeps its value -- tsh refused the name first and made
# nothing (finding 584).
d=$(mktemp -d); cd "$d" || exit
{
( readonly v=1
  exec {v}>made; echo "rc=$?"; ls
  echo data > full; exec {v}>full; echo "rc=$?"; wc -c < full
  echo data > more; exec {v}>>more; echo "rc=$?"; wc -c < more )
u=keep; exec {u}<nosuch; echo "rc=$? $u"
unset u; exec {u}<nosuch; echo "rc=$? ${u-unset}"
} 2>&1 | sed 's/^.*line [0-9]*: //'
cd /; rm -rf "$d"
