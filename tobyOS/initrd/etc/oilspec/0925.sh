cd $TMP
printf "cmd %s\n" {1..10} > tmp
HISTFILE=tmp
HISTFILESIZE=5
cat tmp | wc -l
