case $SH in dash) exit ;; esac

echo {myvar}>/dev/stdout
# Bash chooses fds starting with 10 here, osh with 100, and there can already
# be some open fds, so compare further fds against this one
starting_fd=$myvar

echo x={myvar}>/dev/stdout
echo $((myvar-starting_fd))
echo x={myvar} >/dev/stdout
echo $((myvar-starting_fd))
echo x= {myvar}>/dev/stdout
echo $((myvar-starting_fd))

echo +{myvar}>/dev/stdout
echo $((myvar-starting_fd))
echo +{myvar} >/dev/stdout
echo $((myvar-starting_fd))
echo + {myvar}>/dev/stdout
echo $((myvar-starting_fd))
