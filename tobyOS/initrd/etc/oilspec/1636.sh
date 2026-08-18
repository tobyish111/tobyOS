$SH -c '
echo ${undef?message}
echo inside=$?
'
echo outside=$?

$SH -c '
case ${undef?message} in 
  (*) echo hi ;;
esac
echo inside=$?
'
echo outside=$?
