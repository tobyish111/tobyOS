set -- -h -c
while getopts "hc:" opt; do
  case $opt in
    h) FLAG_h=1 ;;
    c) FLAG_c="$OPTARG" ;;
    '?') echo ERROR $OPTIND; exit 2; ;;
  esac
done
echo status=$?
