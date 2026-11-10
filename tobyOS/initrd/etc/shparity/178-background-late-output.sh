# A background job's output that arrives AFTER the shell has exited is still
# the script's output, and the gate waits for it (M1, finding 31): neither
# shell waits for the job, so the harness used to read the files as soon as
# the shell was gone, and a line written after that was not there -- fixture
# 71 lost "trailing" once that way. The shell now runs in a process group of
# its own and the case ends when that group is empty.
echo first
(sleep 0.3; echo late) &
