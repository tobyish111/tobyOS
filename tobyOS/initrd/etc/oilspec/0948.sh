sleep 0.1 &

# OSH doesn't validate this, but that could be useful for non-portable signals,
# which we don't have a name for.

kill -s 9999 %%
echo kill=$?

wait
echo wait=$?
