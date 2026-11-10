shopt -s expand_aliases
alias e_=echo
>$TMP/alias1.txt e_ 1
e_ >$TMP/alias2.txt 2
e_ 3 >$TMP/alias3.txt
cat $TMP/alias1.txt $TMP/alias2.txt $TMP/alias3.txt
