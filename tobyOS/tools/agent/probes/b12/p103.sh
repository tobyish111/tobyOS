exec 2>&1
sleep 1 & kill -9 $!; sleep 0.3 | cat
echo "a next"
sleep 1 & kill -9 $!; sleep 0.3 | cat; echo "b same"
echo "b next"
sleep 1 & kill -9 $!; sleep 0.3
echo "c next"
sleep 1 & kill -9 $!; ( sleep 0.3 )
echo "d next"
sleep 1 & kill -9 $!; sleep 0.3; :
echo "e next"
sleep 1 & kill -9 $!; /bin/sleep 0.3 | /bin/cat
echo "f next"
sleep 1 & kill -9 $!; sleep 0.3 | cat | cat
echo "g next"
sleep 1 & kill -9 $!; sleep 0.3 | cat &
wait $!; echo "h waited"
echo "h next"
sleep 1 & kill -9 $!; cat /dev/null | sleep 0.3
echo "i next"
echo end
