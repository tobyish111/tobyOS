# POSIX 2.14 set -- the shell options that change control flow.
# Each destructive one runs inside a subshell so a failure aborts only that
# subshell and the case keeps going.
( set -e; true; echo "1:errexit-continues"; false; echo "1:not-reached" )
echo "2=$?"
( set -e; false ) ; echo "3=$?"
( set +e; false; echo "4:noerrexit-continues" )

( set -u; x=set; echo "5:$x" )
echo "6=$?"

> nc.txt
( set -C; echo over > nc.txt ) 2>/dev/null
echo "7=$?"
( set +C; echo over2 > nc.txt; echo "8:clobber-ok" )

> gf1.txt
> gf2.txt
( set -f; echo 9:*.txt )
( set +f; echo 10:*.txt )

( set -f; for f in *.txt; do echo "11:$f"; done )
( set +f; for f in *.txt; do echo "12:$f"; done )

set -- a b c
( set -- x y; echo "13:n=$# [$1]" )
echo "14:n=$# [$1]"

( set -o noglob; echo 15:*.txt )
( set +o noglob; echo 16:*.txt )
echo end
