case $SH in mksh|ash) exit ;; esac

# https://github.com/akinomyoga/ble.sh/blob/79beebd928cf9f6506a687d395fd450d027dc4cd/src/util.sh#L578-L582

# @fn ble/array#unshift arr value...
function ble/array#unshift {
  builtin eval -- "$1=(\"\${@:2}\" \"\${$1[@]}\")"
}
# @fn ble/array#shift arr count
function ble/array#shift {
  # Note: Bash 4.3 以下では ${arr[@]:${2:-1}} が offset='${2'
  # length='-1' に解釈されるので、先に算術式展開させる。
  builtin eval -- "$1=(\"\${$1[@]:$((${2:-1}))}\")"
}
# @fn ble/array#reverse arr
function ble/array#reverse {
  builtin eval "
  set -- \"\${$1[@]}\"; $1=()
  local e$1 i$1=\$#
  for e$1; do $1[--i$1]=\"\$e$1\"; done"
}

a=( {1..6} )
echo "${a[@]}"

ble/array#shift a 1
echo "${a[@]}"

ble/array#shift a 2
echo "${a[@]}"

echo ---

ble/array#unshift a 99
echo "${a[@]}"

echo ---

# doesn't work in zsh!
ble/array#reverse a
echo "${a[@]}"




# Note: yash does not support calling function name with '#'
