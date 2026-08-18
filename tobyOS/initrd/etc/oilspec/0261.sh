case $SH in mksh) exit ;; esac

sp1[1]=x
sp1[5]=y
sp1[9]=z

echo "${#sp1[@]}"
sp1[0x7FFFFFFFFFFFFFFF]=a
echo "${#sp1[@]}"
sp1[0x7FFFFFFFFFFFFFFE]=b
echo "${#sp1[@]}"
sp1[0x7FFFFFFFFFFFFFFD]=c
echo "${#sp1[@]}"
