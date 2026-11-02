# A LINE AFTER A KILL DOES NOT WAIT FOR A JOB THE SIGNAL LEAVES ALIVE.
# Without job control an asynchronous command ignores INT and QUIT, so
# `kill -INT %1` leaves it running; the shell may wait at its next line
# for a job a signal ends, never for one it does not -- forty such kills
# take no time worth the name, and the job is still running after them.
# (The sleep before them lets the job's process ignore the two first.)
exec 2>&1
sleep 3 &
sleep 0.5
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
kill -INT %1
kill -QUIT %1
jobs
kill %1
wait
echo "end $?"
