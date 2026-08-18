case a in
  a) echo A ;;
  *) echo star ;;
esac

for x in a b; do
  case $x in
    # the pattern is DYNAMIC and evaluated on every iteration
    $x) echo loop ;;
    *) echo star ;;
  esac
done
