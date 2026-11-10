# fc's unknown option is "fc: -v: invalid option" and its usage, status 2
# -- tsh said "bad option '-v'" (finding 523).
exec 2>&1
fc -v; echo "rc=$?"
fc -lz; echo "rc=$?"
