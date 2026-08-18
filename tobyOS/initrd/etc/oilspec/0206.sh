set -o nounset
shopt -s strict_arith || true

declare -A ASSOC=()
echo len=${#ASSOC[@]}

# Check that it really can be used like an associative array
ASSOC['k']='32'
echo len=${#ASSOC[@]}

# bash allows a variable to be an associative array AND unset, while OSH
# doesn't
set +o nounset
declare -A u
echo unset len=${#u[@]}
