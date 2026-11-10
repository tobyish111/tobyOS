s=$'héllo世'   # h e-acute l l o (U+4E16): 6 chars, 9 bytes
t=$'é'
show() {
  echo "len=${#s} t=${#t}"
  echo "sub=[${s:1:2}] neg=[${s: -1}] off=[${s:5}]"
  echo "hash=[${s#?}] pct=[${s%?}] hh=[${s##*l}] pp=[${s%%l*}]"
  echo "rep=[${s/?/X}] all=[${s//?/.}] anch=[${s/#h?/Z}]"
  echo "up=[${s^^}] lo=[${t,,}] tog=[${s~~}] first=[${t^}]"
  [[ $s == h?llo? ]] && echo "match-? yes" || echo "match-? no"
  [[ $t == [[:alpha:]] ]] && echo "alpha yes" || echo "alpha no"
  [[ $t == [[:lower:]] ]] && echo "lower yes" || echo "lower no"
  case $s in h??llo*) echo "case-?? yes";; *) echo "case-?? no";; esac
  printf '[%3s][%-4s][%.2s][%c]\n' "$t" "$t" "$s" "$s"
  printf '%q\n' "$s"
  a=("$s" "$t"); echo "alen=${#a[0]} ${#a[1]}"
  IFS= read -r -n 2 x <<< "$s"; echo "readn=[$x]"
  IFS= read -r -N 2 y <<< "$s"; echo "readN=[$y]"
  u="${s:2}"; echo "slice2=[$u] ${#u}"
  echo "rev=[${s: -3:2}]"
  set -- "$s"; echo "pos=${#1} [${1:0:2}]"
  bad=$'a\xffb'; echo "bad=${#bad} [${bad:1:1}]" | od -c | head -2
}
echo "== C"
show
echo "== LANG=C.UTF-8"
LANG=C.UTF-8
show
echo "== LC_ALL=C over LANG"
LC_ALL=C
show
echo "== unset LC_ALL, LC_CTYPE=en_US.UTF-8, LANG=C"
unset LC_ALL; LANG=C; LC_CTYPE=en_US.UTF-8
show
