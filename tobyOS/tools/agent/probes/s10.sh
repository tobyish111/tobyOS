# S10: what a command that cannot be found says, and its status.
nosuchcmd_xyz; echo "rc=$?"
./nosuch_rel; echo "rc=$?"
/tmp/nosuch/abs; echo "rc=$?"
nosuchcmd_xyz | cat; echo "pipe rc=$?"
echo x | ./nosuch_rel; echo "pipe rc=$?"
nosuchcmd_xyz 2>/dev/null; echo "quiet rc=$?"
nosuchcmd_xyz 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
( exec nosuchcmd_xyz ); echo "exec rc=$?"
command nosuchcmd_xyz; echo "command rc=$?"
""; echo "empty rc=$?"
x=1 nosuchcmd_xyz; echo "prefixed rc=$?"
f() { nosuch_in_fn; }; f; echo "fn rc=$?"
mkdir -p /tmp/s10d; /tmp/s10d; echo "dir rc=$?"; rmdir /tmp/s10d
echo done
