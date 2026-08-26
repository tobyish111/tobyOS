echo hello > OSCFLAGS
for x in `cat OSCFLAGS` world; do
  echo $x
done > OSCFLAGS
cat OSCFLAGS
