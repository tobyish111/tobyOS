# Every help topic as bash prints it: -s the synopsis, -d the first line,
# the long doc, and -m the manual page (whose IMPLEMENTATION section names
# the version and MACHTYPE -- tsh's own -- so that line is left out).
for t in $(compgen -A helptopic); do
  echo "== $t"
  help -s "$t"; echo "rc=$?"
  help -d "$t"; echo "rc=$?"
  help "$t"; echo "rc=$?"
  help -m "$t" | while IFS= read -r l; do
    case $l in *'GNU bash, version'*) ;; *) printf '%s\n' "$l" ;; esac
  done
done
echo "== all at once"
help -s '*'; echo "rc=$?"
help -d '*'; echo "rc=$?"
