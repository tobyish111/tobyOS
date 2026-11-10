cd $TMP
HISTFILE=tmp
printf "cmd orig%s\n" {1..3} > tmp
history -c
history -r
history -d 1
history | grep orig1 > /dev/null
echo "status=$?"
