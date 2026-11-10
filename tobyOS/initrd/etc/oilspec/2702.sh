set -- "1 2" "3  4"

IFS=
argv.py $*
argv.py "$*"
