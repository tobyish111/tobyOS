# This was inspired by #416, although that symptom there was timing, so it's
# technically not a regression test.  It's hard to test timing.

{ sleep 0.1; exit 42; } &
echo begin
! true
echo end
wait $!
echo status=$?
