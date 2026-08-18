case $SH in
  *dash|*mksh)
    echo N-I
    exit
    ;;
esac
shopt -s nounset
echo status=$?

# get rid of extra space in bash output
set -o | grep nounset | sed 's/[ \t]\+/ /g'
