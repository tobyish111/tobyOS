n=$($SH --help | wc -l)
if test $n -gt 0; then
  echo yes
fi
