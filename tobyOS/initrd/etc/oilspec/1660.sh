argv.py *.ZZ
shopt -s failglob
argv.py *.ZZ  # nothing is printed, not []
echo status=$?
