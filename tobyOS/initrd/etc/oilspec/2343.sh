( echo 1 ) > a.txt
( env echo 2 ) > b.txt
( env echo 3; ) > c.txt  # Sentence in LST
( echo 4; echo 5 ) > d.txt
echo status=$?
cat a.txt b.txt c.txt d.txt
