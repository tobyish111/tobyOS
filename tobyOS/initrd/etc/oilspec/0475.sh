# foreground
exit 55 | ( cat; exit 99 )
echo fore status=$? pipestatus=${PIPESTATUS[@]}

# background
exit 44 | ( cat; exit 88 ) &
echo back status=$? pipestatus=${PIPESTATUS[@]}

# wait for pipeline
wait %+
#wait %1
#wait $!
echo wait status=$? pipestatus=${PIPESTATUS[@]}
echo

# wait for non-pipeline
( exit 77 ) &
wait %+
echo wait status=$? pipestatus=${PIPESTATUS[@]}
