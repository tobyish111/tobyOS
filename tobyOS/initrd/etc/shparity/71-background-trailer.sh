# A BACKGROUNDED COMMAND STILL GETS TO RUN. tobyOS never reparents an orphan
# in the initial namespace, so a child that had not been scheduled when the
# script ended simply never ran and its output was lost entirely.
# One pending child at a time: the order of two is nobody's contract.
echo word_a & echo word_b
wait
echo middle
echo trailing &
