case $SH in *osh) echo soil-ci-buster-slim-bug; exit ;; esac

# This doesn't make a difference on my local machine?
# Is the underlying issue how libc fnmatch() respects Unicode?

#LC_ALL=C
#LC_ALL=C.UTF-8

c=$(printf \\377)

# OSH prints -1 here
#echo "${#c}"

case $c in
  '')   echo a ;;
  "$c") echo b ;;
esac

case "$c" in
  '')   echo a ;;
  "$c") echo b ;;
esac
