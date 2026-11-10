trap 'echo line=$LINENO' ERR

false || false || false
echo ok

false && false
echo ok
