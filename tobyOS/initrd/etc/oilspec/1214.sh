trap 'echo line=$LINENO' ERR

x=$(false)

[[ a == b ]]

(( 0 ))
echo ok
