# NOTE: Alpine doesn't appear to use this, but bash-completion does.
FLAG_h=0
FLAG_c=''
arg=''
set -- A B C
while getopts "hc:" opt -h -c foo x y z; do
  case $opt in
    h) FLAG_h=1 ;;
    c) FLAG_c="$OPTARG" ;;
  esac
done
echo h=$FLAG_h c=$FLAG_c optind=$OPTIND argv=$@
