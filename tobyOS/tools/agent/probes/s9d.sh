# S9: plain wait's error words (bash 5.2)
wait 99999; echo "unknown pid: $?"
wait %7; echo "unknown job: $?"
wait bad; echo "garbage: $?"
wait -- 99999; echo "after --: $?"
(exit 3) & wait $! $!; echo "same pid twice: $?"
echo done
