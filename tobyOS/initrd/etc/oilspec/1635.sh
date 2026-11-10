$SH -c '
case $(( 42 / 0 )) in
  (*) echo hi ;;
esac
echo inside=$?
'
echo outside=$?

echo ---

$SH -c '
case foo in
  ( $(( 42 / 0 )) )
    echo hi
    ;;
esac
echo inside=$?
'
echo outside=$?
