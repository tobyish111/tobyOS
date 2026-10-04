s=$'été'; z=$'straße'; dz=$'ǅx'; g=$'Αβ'
probe() {
  printf '[%c][%c]\n' "$s" "${g}"
  printf '%s\n' "${s@Q}" "${g@Q}"
  declare -p s g
  ( set -x; : "$s" ) 2>&1
  echo "up=[${z^^}] [${dz,,}] [${dz^^}] [${g^^}] [${g,,}]"
  IFS=$'é'; set -- x${s}y; echo "ifs=$# [$1][$2][$3]"; unset IFS
  mkdir -p gq && cd gq && touch $'éa' ab && echo "glob=[" ?a "]" && cd .. && rm -rf gq
  [[ $s =~ ^.t.$ ]] && echo "re yes" || echo "re no"
  [[ $s =~ ^..t ]] && echo "re2 yes" || echo "re2 no"
  shopt -s nocasematch; [[ $s == $'ÉTÉ' ]] && echo "nocase yes" || echo "nocase no"; shopt -u nocasematch
  [[ $'é' == [[:upper:]] ]] && echo "upper yes" || echo "upper no"
  [[ $'É' == [[:upper:]] ]] && echo "Upper yes" || echo "Upper no"
  [[ $'世' == [[:alpha:]] ]] && echo "cjk alpha yes" || echo "cjk alpha no"
  [[ $' ' == [[:space:]] ]] && echo "nbsp space yes" || echo "nbsp space no"
  [[ $' ' == [[:blank:]] ]] && echo "emsp blank yes" || echo "emsp blank no"
  [[ $'é' == [a-z] ]] && echo "range yes" || echo "range no"
  [[ $'é' == [$'à'-$'ÿ'] ]] && echo "range2 yes" || echo "range2 no"
  bad=$'\xc3('; echo "bad2=${#bad}"
  echo "cut=[${s:0:1}]" | od -An -tx1
  x=$'é'; echo "fmt=$(printf '%5.1s|' "$x")"
}
echo "== default"; probe
echo "== LANG=xx_YY.whatever"; LANG=xx_YY.whatever; echo "len=${#s}"
echo "== LANG=POSIX"; LANG=POSIX; echo "len=${#s}"
echo "== LANG= (empty)"; LANG=; echo "len=${#s}"
unset LANG
echo "== LC_ALL=C prefix"; LC_ALL=C printf '%q\n' "$s"; LC_ALL=C eval 'echo "eval len=${#s}"'; echo "after len=${#s}"
f() { local LC_ALL=C; echo "in f len=${#s}"; }; f; echo "after f len=${#s}"
echo "== export LC_ALL=C"; export LC_ALL=C; echo "len=${#s}"; probe
