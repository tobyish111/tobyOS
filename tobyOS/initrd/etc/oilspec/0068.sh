case $SH in mksh|zsh) exit ;; esac
s=
a=()
declare -A d=([lemon]=yellow)

s+=(1)
s+=([melon]=green)

a+=lime
a+=([1]=banana)

d+=orange
d+=(0)

true
