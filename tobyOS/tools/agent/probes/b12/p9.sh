pa() { printf '['; for x in "$@"; do printf '<%s>' "$x"; done; printf ']\n'; }
for ifsmode in unset default null x spx; do
  case $ifsmode in unset) unset IFS;; default) IFS=$' \t\n';; null) IFS='';; x) IFS=x;; spx) IFS=' x';; esac
  set -- a 'b c' '' 'dxe' ''
  arr=(a 'b c' '' 'dxe' '')
  echo "== IFS=$ifsmode"
  pa $@; pa $*; pa "$@"; pa "$*"; pa =$@=; pa =$*=; pa "=$@="; pa "=$*="
  pa ${arr[@]}; pa ${arr[*]}; pa "${arr[@]}"; pa "${arr[*]}"; pa =${arr[@]}=; pa =${arr[*]}=
  pa ${!arr[@]}; pa ${!arr[*]}; pa "${!arr[@]}"; pa "${!arr[*]}"
  s='1 2'; pa $s"${arr[@]}"_"$@"; pa $s$@; pa "$s"$@
  v=$@; pa "$v"; v=$*; pa "$v"; v="$*"; pa "$v"; v=${arr[*]}; pa "$v"
  set --; pa ${@-minus} ${@+plus} ${@:-minus} ${@:+plus} ${*-m} ${*+p}; pa "$@" "$*" $@ $*
  set -- ''; pa ${@-minus} ${@+plus} ${@:-minus} ${@:+plus}; pa "$@" "$*"
done
