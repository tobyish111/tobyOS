# -N is true when the file was modified since it was last read -- the mtime
# later than the atime, to the nanosecond -- and -O and -G when the
# effective user and group own it. tsh had -N false and -O and -G true for
# every file (bash's test.tests; finding 294).
rm -f ./tn1; touch ./tn1; [ -N ./tn1 ]; echo "fresh=$?"
touch -a -d '2020-01-01 00:00:00' ./tn1; [ -N ./tn1 ]; echo "atime-older=$?"
touch -m -d '2019-01-01 00:00:00' ./tn1; [ -N ./tn1 ]; echo "mtime-older=$?"
touch -a -d '2019-01-01 00:00:00.5' ./tn1; touch -m -d '2019-01-01 00:00:00.7' ./tn1
[ -N ./tn1 ]; echo "subsecond=$?"; [[ -N ./tn1 ]]; echo "dbr=$?"
[ -O ./tn1 ]; echo "O=$?"; [ -G ./tn1 ]; echo "G=$?"
[ -N ./missing ]; echo "missing=$?"
rm -f ./tn1
