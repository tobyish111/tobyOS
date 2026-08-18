case $SH in dash) exit ;; esac

# This test case is sorta "infected" because spec-common.sh sets LC_ALL=C.UTF-8
#
# For some reason mksh behaves differently
#
# See demo/04-unicode.sh

#echo $LC_ALL
unset LC_ALL 

# note: this may depend on the CI machine config
LANG=en_US.UTF-8

#LC_ALL=en_US.UTF-8

for s in $'\u03bc' $'\U00010000'; do
  LC_ALL=
  echo "len=${#s}"

  LC_ALL=C
  echo "len=${#s}"

  echo
done
