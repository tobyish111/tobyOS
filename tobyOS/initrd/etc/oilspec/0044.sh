cd $TMP
shopt -s expand_aliases
echo foo bar > tmp.txt
alias a=argv.py
a `cat tmp.txt`
