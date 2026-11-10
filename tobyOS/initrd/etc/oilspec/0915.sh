# NB: Based on line ranges, not contents

cd $TMP

printf "cmd orig%s\n" {1..10} > tmp1
cp tmp1 tmp2
printf "cmd new%s\n" {1..10} >> tmp2

history -c
HISTFILE=tmp1 history -r
HISTFILE=tmp2 history -n

history | grep orig | wc -l
history | grep new | wc -l
