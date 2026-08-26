$SH -c '
set -o nounset
case ${undef} in 
  (*) echo hi ;;
esac
echo inside=$?
'
echo outside=$?
