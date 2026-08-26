set -- '1 2' '3 4'
argv.py X${unset=x"$@"x}X
argv.py "$unset"
