flags=''
case $SH in
  bash*|*osh)
    flags='--rcfile /dev/null'
    ;;
esac  
$SH $flags -i -c 'var=)'
