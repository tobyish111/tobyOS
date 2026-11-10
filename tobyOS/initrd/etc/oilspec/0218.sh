declare -A a=()
a['symbol1']=\'\'
a['symbol2']='"'
a['symbol3']='()<>&|'
a['symbol4']='[]*?'
echo "[${a[@]@Q}]"
echo "[${a[*]@Q}]"
