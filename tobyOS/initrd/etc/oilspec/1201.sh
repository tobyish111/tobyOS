if test -f /nope; then echo file exists; fi

trap 'echo err' ERR
#trap 'echo line=$LINENO' ERR

if test -f /nope; then echo file exists; fi
