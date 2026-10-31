# With extdebug on, a DEBUG trap that returns non-zero skips the command it
# ran before -- tsh ran it anyway (finding 518).
exec 2>&1
shopt -s extdebug
st=0
tr() { return $st; }
trap 'tr' DEBUG
x=1
st=2
x=2
st=0
echo "x is $x"
st=1
echo skipped
st=0
echo shown
trap - DEBUG
shopt -u extdebug
st=1
trap 'tr' DEBUG
echo not-skipped-without-extdebug
trap - DEBUG
