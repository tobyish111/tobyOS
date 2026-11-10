setopt SH_WORD_SPLIT
#set -x

set -- one "" two

IFS=x

argv.py $@

for i in $@; do
  echo -$i-
done
