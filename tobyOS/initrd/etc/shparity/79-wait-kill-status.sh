# POSIX wait and kill -l: statuses and the signal name table. Nothing here
# waits on a real child, so nothing here can race.
wait
echo "1=$?"
wait 99999
echo "2=$?"
kill -l 15
kill -l 1
kill -l 9
kill -l 2
echo "3=$?"
kill 999999
echo "4=$?"
kill -l 143
echo end
