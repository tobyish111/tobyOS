exec 2>&1 </dev/null
e=$'é'; a2=$'α'; cjk=$'世'
show() { printf '%d:' $#; for w; do printf '[%s]' "$w"; done; echo; }
echo "== words"
IFS=$e; v="x${e}t${e}y"; show $v; unset IFS
IFS=$e; v="${e}x${e}${e}y${e}"; show $v; unset IFS
IFS=" $e"; v="a $e b"; show $v; unset IFS
IFS=" $e"; v="a ${e} ${e}b"; show $v; unset IFS
IFS="$e "; v=" a${e}  b ${e}"; show $v; unset IFS
IFS="${a2}${e}"; v="1${e}2${a2}3"; show $v; unset IFS
IFS=$e; v=$'x\xc3y\xa9z'; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\xc3'; v="x${e}y"; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\xa9'; v="x${e}y"; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\xa9'"$e"; v="x${e}y"; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$cjk; v="a${cjk}b"; show $v; unset IFS
IFS="${e}:"; v="a:b${e}c"; show $v; unset IFS
echo "== join"
set -- a b c
IFS=$e; echo "[$*]"; x=("$@"); echo "[${x[*]}]"; unset IFS
IFS="${cjk}x"; echo "[$*]"; unset IFS
IFS=$'\xc3'; echo "[$*]" | od -An -c | tr -s ' '; unset IFS
IFS=$e; y="${*}"; echo "y=[$y]"; unset IFS
echo "== read"
IFS=$e read -r p q r <<< "1${e}2${e}3${e}4"; echo "[$p][$q][$r]"
IFS=$e read -r -a arr <<< "${e}1${e}${e}2"; echo "${#arr[@]} [${arr[0]}][${arr[1]}][${arr[2]}]"
IFS=" $e" read -r p q <<< "  a $e b  "; echo "[$p][$q]"
IFS=$e read -r p q <<< "a${e}"; echo "[$p][$q]"
IFS=$e read -r p q <<< "a${e}b${e}"; echo "[$p][$q]"
IFS=$e read -r p <<< "a${e}"; echo "[$p]"
echo "== array literal"
IFS=$e; v="m${e}n${e}o"; arr2=($v); echo "${#arr2[@]} [${arr2[0]}][${arr2[1]}][${arr2[2]}]"; unset IFS
echo "== isspace whitespace"
IFS=$'\r:'; v=$'\ra\r\rb:\rc\r'; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\v'; v=$'\va\v\vb\v'; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\f:'; v=$'a\f:\fb'; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\r' read -r p q <<< $'\r\ra\r\rb\r'; echo "[$p][$q]" | od -An -c | tr -s ' '
IFS=$'\v' read -r -a arr <<< $'\va\v\vb\v'; echo "${#arr[@]} [${arr[0]}][${arr[1]}]" | od -An -c | tr -s ' '
IFS=$'\r:' read -r p q r <<< $'a\r:\rb:c'; echo "[$p][$q][$r]" | od -An -c | tr -s ' '
IFS=$'\r'; v=$'\r\rm\r\rn'; arr3=($v); echo "${#arr3[@]}"; unset IFS
echo "== array literal empties"
IFS=:; v="a::b"; arr4=($v); echo "${#arr4[@]} [${arr4[1]}]"; unset IFS
IFS=:; v=":a"; arr4=($v); echo "${#arr4[@]} [${arr4[0]}]"; unset IFS
IFS=:; v="a:"; arr4=($v); echo "${#arr4[@]}"; unset IFS
IFS=": "; v="a : b"; arr4=($v); echo "${#arr4[@]}"; unset IFS
IFS=:; v="a::b"; arr4=($v x); echo "${#arr4[@]}"; unset IFS
IFS=:; v="a:"; arr4=($v"" x); echo "${#arr4[@]}"; unset IFS
IFS=:; v="a::b"; set -- $v; echo "$#"; unset IFS
echo "== read escapes"
IFS=' ' read x y <<< 'a b c\ '; echo "[$y]"
IFS=' ' read x <<< 'a\ '; echo "[$x]"
IFS=' ' read x y <<< 'a\ b c'; echo "[$x][$y]"
IFS=: read x y <<< 'a\:b:c\:'; echo "[$x][$y]"
echo "== C locale"
LC_ALL=C; IFS=$e; v="x${e}t${e}y"; show $v; unset IFS; unset LC_ALL
LC_ALL=C; IFS=$e; set -- a b; echo "[$*]" | od -An -c | tr -s ' '; unset IFS; unset LC_ALL
echo end
