set -e
argv.py *.ZZ
shopt -s failglob
argv.py *.ZZ
echo status=$?
