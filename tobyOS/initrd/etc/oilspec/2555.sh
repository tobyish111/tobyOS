# note: this also works in zsh

declare -A arr=()
echo ${#arr[@]}
: ${arr['k']=x}
echo ${#arr[@]}
