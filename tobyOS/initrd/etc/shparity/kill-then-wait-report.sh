# A background job a signal ends takes time to die and tell the shell, and
# bash reaches the line after its own `kill` sooner: `wait`'s own 2> takes
# the job's report (Oils 0488). tsh's look straight after the kill saw the
# death about one try in twenty and reported it on the script's stderr,
# which this case sends away, so the count came up short (finding 636;
# logs/b46 kwr8-rate.txt: 5 runs of 20 short, 0 after). Each try is its own
# lines -- a loop's body reads no line between -- and there are few of
# them: bash itself loses the race about once in a thousand tries.
exec 2>/dev/null
sleep 1 &
kill -HUP $!
wait $! 2>e1
sleep 1 &
kill -HUP $!
wait $! 2>e2
sleep 1 &
kill -HUP $!
wait $! 2>e3
sleep 1 &
kill -HUP %1
wait %1 2>e4
sleep 1 &
kill -HUP %%
wait %% 2>e5
sleep 1 &
p=$!
kill -HUP $p
wait $p 2>e6
sleep 1 & kill -HUP $!
wait $! 2>e7
sleep 1 &
kill -s HUP $!
wait $! 2>e8
n=0
for i in 1 2 3 4 5 6 7 8; do
    grep -q Hangup e$i && n=$((n+1))
done
echo "reports in wait's own 2>: $n of 8"
