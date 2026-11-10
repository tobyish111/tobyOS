py-repr() {
  python2 -c 'import sys; print repr(sys.argv[1])'  "$@"
}

py-repr $'\udc00'

py-repr $'\U0000dc00' 
