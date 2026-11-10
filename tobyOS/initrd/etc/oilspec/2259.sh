# note: test/spec-common.sh sets LC_ALL
unset LC_ALL

touch _x_ _μ_

LC_ALL=C       $SH -c 'echo LC_ALL _?_'
LC_ALL=C.UTF-8 $SH -c 'echo LC_ALL _?_'
echo

LC_CTYPE=C       $SH -c 'echo LC_CTYPE _?_'
LC_CTYPE=C.UTF-8 $SH -c 'echo LC_CTYPE _?_'
echo

LC_COLLATE=C       $SH -c 'echo LC_COLLATE _?_'
LC_COLLATE=C.UTF-8 $SH -c 'echo LC_COLLATE _?_'
echo

LANG=C       $SH -c 'echo LANG _?_'
LANG=C.UTF-8 $SH -c 'echo LANG _?_'
