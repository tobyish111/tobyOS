#complete -F f cmd

# Alias for complete -p
complete > /dev/null  # ignore OSH output for now
echo status=$?

# But this is an error
complete -F f
echo status=$?
