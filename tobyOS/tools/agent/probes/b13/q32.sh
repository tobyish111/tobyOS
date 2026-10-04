exec 2>&1 </dev/null
mkdir -p /tmp/q32d && cd /tmp/q32d && rm -f * && touch ax bx 'a*x' c
show() { printf '%d:' $#; for w; do printf '[%s]' "$w"; done; echo; }
echo "== argv"
show *"x"
show "a"*
show 'a*'x
show "*"x
show a"*"*
v='a b'; show $v"" "$v"x x"$v"y
v='*'; show $v"x" "$v"
echo "== array"
a=(*"x"); show "${a[@]}"
a=("a"*); show "${a[@]}"
a=('a*'x); show "${a[@]}"
a=("*"x); show "${a[@]}"
a=(a"*"*); show "${a[@]}"
v='a b'; a=($v"" "$v"x x"$v"y); show "${a[@]}"
v='*'; a=($v"x" "$v"); show "${a[@]}"
v='p q'; a=(x"$v"$v); show "${a[@]}"
IFS=:; v="a:"; a=($v"" x); show "${a[@]}"; unset IFS
v=''; a=($v"" x); show "${a[@]}"
v=''; a=("$v"$v); show "${a[@]}"
v='a b'; a=(${v}'') ; show "${a[@]}"
v='  '; a=($v""); show "${a[@]}"
v=' a '; a=(""$v""); show "${a[@]}"
cd /; rm -rf /tmp/q32d
echo end
