d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# A QUOTE IN AN ARRAY-LITERAL ELEMENT PROTECTS ITS OWN SPAN ONLY (batch
# 13): a=($v"") still splits $v, a=(*"x") still globs, a=("*"x) does not.
# A quote anywhere turned splitting and globbing off for the whole
# element. (logs/b13 q32)
touch ax bx 'a*x' c
show() { printf '%d:' $#; for w in "$@"; do printf '[%s]' "$w"; done; echo; }
a=(*"x"); show "${a[@]}"
a=("a"*); show "${a[@]}"
a=('a*'x); show "${a[@]}"
a=("*"x); show "${a[@]}"
a=(a"*"*); show "${a[@]}"
v='a b'; a=($v"" "$v"x x"$v"y); show "${a[@]}"
v='*'; a=($v"x" "$v"); show "${a[@]}"
v='p q'; a=(x"$v"$v); show "${a[@]}"
v=''; a=($v"" x); show "${a[@]}"
v='a b'; a=(${v}''); show "${a[@]}"
v='  '; a=($v""); show "${a[@]}"
v=' a '; a=(""$v""); show "${a[@]}"
echo end
