# INFINITE LOOP in ash!
case $SH in ash) exit ;; esac

g='*'
v='a*b'
echo ${v//"$g"/-}
echo ${v//$g/-}
