s=dollar0
echo 'echo $@' > $s
chmod +x $s
$SH $s a b c
