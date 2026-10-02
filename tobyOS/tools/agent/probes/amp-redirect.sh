echo out &> o.txt; echo st=$?; cat o.txt
ls /nonexist-zz &> e.txt; echo st=$?; wc -l < e.txt
echo more &>> o.txt; cat o.txt
{ echo a; echo b >&2; } &> g.txt; sort g.txt
f() { echo fo; echo fe >&2; }; f &> h.txt; sort h.txt
2&>1; echo st=$?; cat 1 | wc -l
set -o noclobber; echo x &> o.txt; echo st=$?; set +o noclobber
echo bg & wait; echo after
rm -f o.txt e.txt g.txt h.txt 1
