f=${TMPDIR:-/tmp}/exdd.$$
exec -- 3>"$f"
echo E01 >&3
exec 3>&-
cat "$f"
exec -- 4>&1
echo E02 1>&4
exec 4>&-
( exec -- echo E03 sub )
echo "E03 st=$?"
exec --; echo "E04 st=$?"
( exec -- false ); echo "E05 st=$?"
rm -f "$f"
echo done
