HOME=$TMP
echo hi > ~/tilde1.txt
cat $HOME/tilde1.txt | wc -c
