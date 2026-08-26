trap 'echo USR1 trap status=$?; ( exit 42 )' USR1

echo before=$?

# Equivalent to 'kill -USR1 $$' except OSH doesn't have "kill" yet.
# /bin/kill doesn't exist on Debian unless 'procps' is installed.
sh -c "kill -USR1 $$"
echo after=$?
