# A CHLD trap runs once for each child: three that end during a wait are
# three runs, a two-stage pipeline two -- tsh ran it once a batch
# (finding 542).
trap 'echo chld' CHLD
sleep 0.3 & sleep 0.2 & sleep 0.1 &
wait
echo after-wait
/bin/true
echo after-true
/bin/true | /bin/true
echo after-pipe
trap - CHLD
