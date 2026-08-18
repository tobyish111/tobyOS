cat /dev/urandom | sleep 0.1
echo ${PIPESTATUS[@]}

# hm bash gives '1 0' which seems wrong
