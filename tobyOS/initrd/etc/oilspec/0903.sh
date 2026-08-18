OPTIND=-1
getopts a: foo
echo status=$?

OPTIND=0
getopts a: foo
echo status=$?
