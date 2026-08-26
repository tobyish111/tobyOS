debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

name=foo.py

case $name in 
  *.py)
    echo python
    ;;
  *.sh)
    echo shell
    ;;
esac
echo ok
