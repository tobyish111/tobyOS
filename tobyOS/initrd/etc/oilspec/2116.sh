# Notes:
# - 7/2021: descriptor 7 seems to work on all CI systems.  The process state
#   isn't clean, but we could probably close it in OSH?
# - dash doesn't allow file descriptors greater than 9.  (This is a good
#   thing, because the bash chapter in AOSA book mentions that juggling user
#   vs.  system file descriptors is a huge pain.)
# - But somehow running in parallel under spec-runner.sh changes whether
#   descriptor 3 is open.  e.g. 'echo hi 1>&3'.  Possibly because of
#   /usr/bin/time.  The _tmp/spec/*.task.txt file gets corrupted!
# - Oh this is because I use time --output-file.  That opens descriptor 3.  And
#   then time forks the shell script.  The file descriptor table is inherited.
#   - You actually have to set the file descriptor to something.  What do
#   configure and debootstrap too?

opened=$(ls /proc/$$/fd)
if echo "$opened" | egrep '^7$'; then
  echo "FD 7 shouldn't be open"
  echo "OPENED:"
  echo "$opened"
fi

echo hi 1>&7
