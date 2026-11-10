d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# FIELD SPLITTING BY CHARACTER (batch 13). bash finds a separator a
# character at a time -- a multibyte IFS character as a whole, a lone byte
# that starts no character by IFS's bytes -- and an IFS holding an invalid
# sequence matches no multibyte character. IFS whitespace is isspace, \v \f
# \r included. "$*" joins with IFS's first CHARACTER. read splits the same
# way, with its own rule for the last variable; a=($v) keeps the empty
# fields word splitting keeps. tsh compared bytes and knew three
# whitespace characters. (logs/b13 q31)
e=$'é'
show() { printf '%d:' $#; for w in "$@"; do printf '[%s]' "$w"; done; echo; }
IFS=$e; v="x${e}t${e}y"; show $v; unset IFS
IFS=" $e"; v="a $e b"; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$e; v=$'x\xc3y\xa9z'; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\xa9'"$e"; v="x${e}y"; show $v | od -An -c | tr -s ' '; unset IFS
IFS=$'\r:'; v=$'\ra\r\rb:\rc\r'; show $v | od -An -c | tr -s ' '; unset IFS
set -- a b c
IFS=$e; echo "[$*]"; x=("$@"); echo "[${x[*]}]"; unset IFS
IFS=$e read -r p q r <<< "1${e}2${e}3${e}4"; echo "[$p][$q][$r]"
IFS=$e read -r -a arr <<< "${e}1${e}${e}2"; echo "${#arr[@]} [${arr[0]}][${arr[1]}][${arr[2]}]"
IFS=$'\r' read -r p q <<< $'\r\ra\r\rb\r'; echo "[$p][$q]" | od -An -c | tr -s ' '
IFS=' ' read x y <<< 'a b c\ '; echo "[$y]"
IFS=:; v="a::b"; a4=($v); echo "${#a4[@]} [${a4[1]}]"; unset IFS
IFS=:; v=":a"; a4=($v); echo "${#a4[@]}"; unset IFS
IFS=:; v="a:"; a4=($v"" x); echo "${#a4[@]}"; unset IFS
echo end
