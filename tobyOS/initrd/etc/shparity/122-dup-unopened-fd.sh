echo B01 >&3; echo "B01 st=$?"
echo B02 2>&4; echo "B02 st=$?"
cat <&5; echo "B03 st=$?"
{ echo B04; } >&6; echo "B04 st=$?"
f=${TMPDIR:-/tmp}/dupfd.$$
exec 3>"$f"; echo B05 >&3; exec 3>&-; cat "$f"
echo B06 >&3; echo "B06 closed st=$?"
( echo B07 >&7 ); echo "B07 sub st=$?"
x=$(echo B08 >&8); echo "B08 [$x] st=$?"
echo B09 >&1
exec 4>"$f"; echo B10 >&4; exec 4>&-; cat "$f"
rm -f "$f"
echo done
