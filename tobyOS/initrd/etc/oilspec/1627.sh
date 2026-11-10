shopt -s extglob
str='x'
[[ 1 == !($str) ]]  && echo TRUE   # glob match
