case $SH in ash|dash|yash) exit 99 ;; esac
v=x
case $SH in
mksh) f1() { \builtin typeset v=1; echo "l:v=$v"; } ;;
*)    f1() { \builtin local   v=1; echo "l:v=$v"; } ;;
esac
f1
echo "g:v=$v"
# Note: ash/dash/yash does not have "builtin"
