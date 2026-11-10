seq 2 3 > myfile
foo=$(< myfile)
argv.py "$foo"
