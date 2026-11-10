#case $SH in mksh|dash) exit ;; esac

sleep 0.5 &
pid=$!
kill -15 %%
echo kill=$?

wait %%
echo wait=$?

# no such job
wait %%
echo wait=$?
