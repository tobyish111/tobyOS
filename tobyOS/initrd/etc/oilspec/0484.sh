sleep 0.1 &
sleep 0.1 | cat &

jobs -p > tmp.txt

cat tmp.txt | wc -l  # 2 lines, one for each job
cat tmp.txt | wc -w  # each line is a single "word"
