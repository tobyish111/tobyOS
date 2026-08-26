cd $TMP
printf "cmd orig%s\n" {1..10} > tmp
HISTFILE=tmp

history -c

history -r
history -r

history | grep orig | wc -l
