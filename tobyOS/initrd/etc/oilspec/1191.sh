debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

for (( i =3 ; i < 5; ++i )); do
  echo i=$i
done

echo ok
