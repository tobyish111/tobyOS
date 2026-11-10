IFS=b
word=abcd
f() { local IFS=c; argv.py $word; }
f
argv.py $word
