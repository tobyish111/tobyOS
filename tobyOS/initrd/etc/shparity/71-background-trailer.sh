# A BACKGROUNDED COMMAND STILL GETS TO RUN. tobyOS never reparents an orphan
# in the initial namespace, so a child that had not been scheduled when the
# script ended simply never ran and its output was lost entirely.
#
# What is asserted here is PRESENCE, not order. `a & b` may print either way
# round -- both shells are entitled to it and both were observed doing it --
# so the ordering is pinned with `wait` rather than left to the scheduler.
echo word_a &
wait
echo word_b
echo middle
echo trailing &
