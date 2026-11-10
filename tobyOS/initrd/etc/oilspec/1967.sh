# POSIX grammar seems to allow this, but bash and dash don't.  Need ;;
foo=a
case $foo in
  a)
  b)
    echo A ;;
  d)
esac
