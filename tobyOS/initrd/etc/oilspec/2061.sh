rm -f myfile
test -f myfile
echo status=$?

>myfile
test -f myfile
echo status=$?



# regression for OSH
