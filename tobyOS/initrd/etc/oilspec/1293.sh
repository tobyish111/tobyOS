umask 0124

umask b=rwx
case $? in
  1) echo error ;;
  2) echo error ;;
  *) echo status=$? ;;
esac

umask  # make sure it hasn't changed
