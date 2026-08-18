case $SH in mksh) exit ;; esac

py-repr() {
  python2 -c 'import sys; print repr(sys.argv[1])'  "$@"
}

e="$(echo -e '\udc00')"
echo status=$?
py-repr "$e"

e="$(echo -e '\U0000dc00')"
echo status=$?
py-repr "$e"

p="$(printf '\udc00')"
echo status=$?
py-repr "$p"

p="$(printf '\U0000dc00')"
echo status=$?
py-repr "$p"
