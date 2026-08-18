# 1. prepare default fd for internal uses
minfd=10
case ${SH##*/} in
(mksh) minfd=24 ;;
(osh) minfd=100 ;;
esac

# 2. prepare first unused fd
fd=$minfd
is_fd_open() { : >&$1; }
while is_fd_open "$fd"; do
  : $((fd+=1))

  # OLD: prevent infinite loop for broken oils-for-unix
  #if test $fd -gt 1000; then
  #  break
  #fi
done

# 3. test
echo foo54 >&$fd
