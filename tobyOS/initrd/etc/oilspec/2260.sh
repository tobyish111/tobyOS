# note: test/spec-common.sh sets LC_ALL
unset LC_ALL

touch _x_ _μ_

{ LC_CTYPE=invalid $SH -c 'echo LC_CTYPE _?_' 
} 2> err.txt

#cat err.txt
wc -l err.txt
