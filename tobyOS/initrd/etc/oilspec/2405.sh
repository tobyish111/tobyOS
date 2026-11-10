case $SH in mksh) exit ;; esac

py-repr() {
  python2 -c 'import sys; print repr(sys.argv[1])'  "$@"
}

e="$(echo -e '\U00110000')"
echo status=$?
py-repr "$e"

p="$(printf '\U00110000')"
echo status=$?
py-repr "$p"
