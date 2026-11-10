# foreground
{ echo hi; exit 55; } | false
echo fore status=$? pipestatus=${PIPESTATUS[@]}

# background
{ echo hi; exit 44; } | false &
echo back status=$? pipestatus=${PIPESTATUS[@]}

# wait for pipeline
wait %+
#wait %1
#wait $!
echo wait status=$? pipestatus=${PIPESTATUS[@]}
