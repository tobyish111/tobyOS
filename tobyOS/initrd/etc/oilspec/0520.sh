v=x
case $SH in
mksh) f() { IFS= eval 'typeset v=1'; echo "l:$v"; } ;;
*)    f() { IFS= eval 'local   v=1'; echo "l:$v"; } ;;
esac
f
echo "g:$v"
