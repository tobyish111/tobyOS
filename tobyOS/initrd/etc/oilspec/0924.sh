cd $TMP
printf "cmd %s\n" {1..10} > tmp
HISTFILE=tmp
history -c
history -r
history | wc -l
HISTSIZE=5
history | wc -l
