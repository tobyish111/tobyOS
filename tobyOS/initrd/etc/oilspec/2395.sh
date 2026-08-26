# example
# https://github.com/NixOS/nixpkgs/blob/master/pkgs/stdenv/generic/setup.sh#L379

declare -i s
s='1 + 2'
echo s=$s

declare -a array=(1 2 3)
declare -i item
item='array[1+1]'
echo item=$item
