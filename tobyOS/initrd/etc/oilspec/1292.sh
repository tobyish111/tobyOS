umask ''
case $? in
  1) echo error ;;
  2) echo error ;;
  *) echo status=$? ;;
esac

umask ' '
case $? in
  1) echo error too ;;
  2) echo error too ;;
  *) echo status=$? ;;
esac
